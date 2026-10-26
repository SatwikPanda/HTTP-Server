#pragma once

#include "proxy/cache_transaction.hpp"
#include "http/date.hpp"
#include "proxy/proxy_test_support.hpp"

namespace cached_test {
using namespace std::chrono_literals;
using proxy::CacheEvent;
using namespace concurrency;
inline auto bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
inline std::string text(std::span<const std::byte> data) { return {reinterpret_cast<const char*>(data.data()), data.size()}; }
inline const std::string date = "Thu, 01 Jan 2026 00:00:00 GMT";
inline std::string response(std::string body = "old", std::string fields = "Cache-Control: max-age=60\r\nETag: \"v1\"\r\n") {
    return "HTTP/1.1 200 OK\r\nDate: " + date + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + fields + "\r\n" + body;
}
inline std::string validation(std::string fields = "ETag: \"v1\"\r\nCache-Control: max-age=120\r\n") {
    return "HTTP/1.1 304 Not Modified\r\nDate: " + date + "\r\n" + fields + "\r\n";
}
struct Environment {
    core::ManualSteadyClock steady{core::SteadyTimePoint{} + 1s};
    core::ManualSystemClock wall{test::take(http::parse_http_date(date, {}))};
    std::shared_ptr<cache::storage::LruStorage> storage;
    std::shared_ptr<proxy::CacheStats> stats{std::make_shared<proxy::CacheStats>()};
    std::shared_ptr<proxy::ProxyStats> forwarding{std::make_shared<proxy::ProxyStats>()};
    std::shared_ptr<const http::TransactionFactory> factory;
    explicit Environment(cache::storage::Limits limits = {}, proxy::CacheProxyConfig config = {})
        : storage(test::take(cache::storage::LruStorage::create(limits))) {
        factory = test::take(proxy::make_cache_factory(storage, config, stats, forwarding));
    }
    cache::Key key(std::string_view path = "/item") const {
        return test::take(http::parse_target("http://127.0.0.1:8080" + std::string(path)));
    }
    cache::Snapshot entry(std::string_view path = "/item") const { return storage->lookup(key(path)).entry; }
};

// Only origin I/O and clocks are scripted. B parses every response and D/C make
// every decision. A step can be paused exactly at a head or work request, making
// overlap deterministic without timing sleeps or fabricated cache behavior.
class Exchange {
public:
    Exchange(Environment& env, std::vector<std::string> responses = {}, std::string extra = {},
             std::string path = "/item", http::Method method = http::Method::GET)
        : environment(env), wires(std::move(responses)) {
        request.method = method; request.target = path.starts_with("http://") ? path : "http://127.0.0.1:8080" + path;
        request.headers.add("Host", "ignored.test");
        if (!extra.empty()) {
            http::RequestParser parser;
            const auto wire = proxy_test::get(request.target, http::to_string(method), true, "HTTP/1.1", extra);
            CHECK(parser.consume(wire).event == http::ParseEvent::complete);
            request = parser.request();
        }
        transaction = test::take(env.factory->create(request, {{{1, 1}, 1}, 256, env.steady, env.wall}));
        step = transaction->on_event(http::StartTransaction{});
    }
    void advance() {
        CHECK(!done);
        if (const auto* action = std::get_if<http::TransactionWork>(&step)) {
            WorkEvent event;
            if (std::holds_alternative<ResolveWork>(action->request)) {
                ++resolves; event.candidates.push_back(net::Endpoint::ipv4_loopback(8080));
            } else if (std::holds_alternative<ConnectWork>(action->request)) {
                ++connections; offset = 0; requests.emplace_back();
            } else if (const auto* write = std::get_if<UpstreamWriteWork>(&action->request)) {
                event.status = WorkStatus::progress; event.transferred = std::min<std::size_t>(7, write->count);
                requests.back() += text(std::span(*write->bytes).subspan(write->offset, event.transferred));
            } else if (const auto* read = std::get_if<UpstreamReadWork>(&action->request)) {
                CHECK(connections > 0 && connections <= wires.size());
                auto& wire = wires[connections - 1];
                const auto count = std::min({read->max_bytes, read_window, wire.size() - offset});
                if (!count) event.status = WorkStatus::eof;
                else {
                    event.status = WorkStatus::progress;
                    const auto part = bytes(wire).subspan(offset, count); offset += count;
                    event.bytes = std::make_shared<const Bytes>(part.begin(), part.end());
                }
            } else { CHECK(std::holds_alternative<UpstreamCloseWork>(action->request)); ++closes; }
            step = transaction->on_event(event);
        } else if (const auto* head = std::get_if<http::ResponseHead>(&step)) {
            heads.push_back(*head);
            if (request.method == http::Method::HEAD || head->response.status_code == 204 || head->response.status_code == 304) {
                done = true; return;
            }
            step = transaction->on_event(http::OutputDrained{body_window});
        } else if (const auto* body = std::get_if<http::ResponseBody>(&step)) {
            output += text(*body->bytes);
            step = transaction->on_event(http::OutputDrained{body_window});
        } else if (const auto* error = std::get_if<http::TransactionFailure>(&step)) {
            failure = static_cast<int>(error->status); done = true;
        } else { CHECK(std::holds_alternative<http::ResponseFinished>(step)); done = true; }
    }
    void until_head() {
        for (unsigned count = 0; count < 100000; ++count) {
            if (std::holds_alternative<http::ResponseHead>(step) || done) return;
            advance();
        }
        CHECK(false);
    }
    void finish() {
        for (unsigned count = 0; count < 100000; ++count) {
            if (done) return;
            advance();
        }
        CHECK(false);
    }
    Environment& environment;
    http::Request request;
    std::unique_ptr<http::Transaction> transaction;
    http::TransactionStep step;
    std::vector<std::string> wires, requests;
    std::vector<http::ResponseHead> heads;
    std::string output;
    std::size_t resolves{}, connections{}, closes{}, offset{}, read_window{43}, body_window{3};
    int failure{};
    bool done{};
};

inline void fill(Environment& env, std::string wire = response(), std::string path = "/item") {
    Exchange exchange(env, {std::move(wire)}, {}, std::move(path)); exchange.finish();
    CHECK(!exchange.failure && exchange.connections == 1);
}
inline http::HttpSessionConfig session(Environment& env) {
    http::HttpSessionConfig config;
    config.application = env.factory; config.max_output_bytes = 256; config.keep_alive = true;
    return config;
}
} // namespace cached_test
