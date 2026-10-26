#pragma once

// Proxy-specific test helpers. Only downstream I/O and the origin are
// controlled here; every proxy decision runs through production code.
#include "fixtures/parallel_io.hpp"
#include "concurrency/work.hpp"
#include "http/http_session.hpp"
#include "http/response_parser.hpp"
#include "proxy/transaction.hpp"

#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace proxy_test {
using namespace std::chrono_literals;

inline auto bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }

// Scripted downstream peer: fixed input, short writes of `write_window`, an
// optional would-block on every Nth write and an optional failure after
// `fail_after` output bytes (client disconnect).
class Channel final : public net::StreamChannel {
public:
    explicit Channel(std::string input, std::size_t write_window = 7, std::size_t block_every = 0)
        : input_(std::move(input)), window_(write_window), block_every_(block_every) {}
    net::ReadResult read_some(std::span<std::byte> output) override {
        if (offset_ == input_.size()) return net::ReadResult::eof();
        const auto count = std::min(output.size(), input_.size() - offset_);
        std::memcpy(output.data(), input_.data() + offset_, count); offset_ += count;
        return net::ReadResult::success(count);
    }
    net::WriteResult write_some(std::span<const std::byte> input) override {
        ++writes;
        if (block_every_ && writes % block_every_ == 0) return net::WriteResult::would_block_result();
        if (output.size() >= fail_after) return net::WriteResult::failure(core::make_error(core::ErrorCategory::connection_reset, "client gone"));
        const auto count = std::min({window_, input.size(), fail_after - output.size()});
        output.append(reinterpret_cast<const char*>(input.data()), count);
        return net::WriteResult::success(count);
    }
    bool is_valid() const noexcept override { return valid_; }
    void close() noexcept override { valid_ = false; }
    std::string output;
    std::size_t writes{};
    std::size_t fail_after{static_cast<std::size_t>(-1)};
private:
    std::string input_;
    std::size_t offset_{}, window_, block_every_;
    bool valid_{true};
};

struct OriginResponse {
    std::vector<std::string> segments;
    bool hold_open{false}; // keep the connection open after the last segment
};

// Accepts one connection per scripted response, in order. Each connection
// reads one request head, writes the segments through `write_window`-sized
// writes and then closes (or holds open until the origin is destroyed).
class MultiOrigin {
public:
    explicit MultiOrigin(std::vector<OriginResponse> responses, std::size_t write_window = 97,
                         net::AddressFamily family = net::AddressFamily::ipv4)
        : listener_(test::listener(family)), endpoint_(test::take(listener_.local_endpoint())),
          responses_(std::move(responses)), window_(write_window), thread_([this] { serve(); }) {}
    ~MultiOrigin() { stop_.store(true); if (thread_.joinable()) thread_.join(); }
    MultiOrigin(const MultiOrigin&) = delete;
    MultiOrigin& operator=(const MultiOrigin&) = delete;
    [[nodiscard]] std::string authority() const {
        return endpoint_.family() == net::AddressFamily::ipv6
            ? "[" + endpoint_.address() + "]:" + std::to_string(endpoint_.port())
            : endpoint_.address() + ":" + std::to_string(endpoint_.port());
    }
    [[nodiscard]] std::string url(std::string_view path = "/") const { return "http://" + authority() + std::string(path); }
    void join() { thread_.join(); CHECK(!error_); }
    [[nodiscard]] unsigned connections() const { return connections_.load(); }
    [[nodiscard]] const std::vector<std::string>& requests() const { return requests_; } // after join only
private:
    bool wait(net::Poller& poller, net::RegistrationToken token, net::Interest interest, net::Deadline deadline) {
        CHECK(poller.modify(token, interest));
        while (!stop_ && test::Clock::now() < deadline) {
            auto events = test::take(poller.wait(std::min(deadline, test::Clock::now() + 25ms)));
            for (const auto& event : events) if (event.token == token && poller.is_current(event)) return true;
        }
        return false;
    }
    void serve() {
        auto poller = test::take(net::Poller::create());
        const auto deadline = test::Clock::now() + 15s;
        std::uint64_t generation = 1;
        for (const auto& response : responses_) {
            auto token = test::take(poller.watch(listener_, net::Interest::read, {1, generation++}));
            net::Socket socket;
            while (wait(poller, token, net::Interest::read, deadline)) {
                auto accepted = listener_.accept();
                if (accepted) { socket = std::move(accepted.value()); break; }
                if (accepted.error().category != core::ErrorCategory::would_block) { error_ = true; break; }
            }
            CHECK(poller.unwatch(token));
            if (!socket.is_valid()) return;
            ++connections_;
            CHECK(socket.set_nonblocking(true));
            token = test::take(poller.watch(socket, net::Interest::read, {2, generation++}));
            std::array<std::byte, 512> input{};
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
                const auto read = socket.read_some(input);
                if (read.is_ok()) request.append(reinterpret_cast<const char*>(input.data()), read.bytes_transferred);
                else if (read.is_eof() || !read.would_block() || !wait(poller, token, net::Interest::read, deadline)) break;
            }
            requests_.push_back(request);
            for (const auto& segment : response.segments) {
                std::size_t offset = 0;
                while (offset < segment.size() && !stop_ && test::Clock::now() < deadline) {
                    const auto data = std::as_bytes(std::span(segment)).subspan(offset, std::min(window_, segment.size() - offset));
                    const auto write = socket.write_some(data);
                    if (write.is_ok()) offset += write.bytes_transferred;
                    else if (!write.would_block() || !wait(poller, token, net::Interest::write, deadline)) break;
                }
            }
            if (response.hold_open) while (!stop_ && test::Clock::now() < deadline) std::this_thread::sleep_for(5ms);
            CHECK(poller.unwatch(token));
        }
    }
    net::Socket listener_;
    net::Endpoint endpoint_;
    std::vector<OriginResponse> responses_;
    std::size_t window_;
    std::atomic<bool> stop_{};
    std::atomic<unsigned> connections_{};
    bool error_{};
    std::vector<std::string> requests_;
    std::thread thread_;
};

inline std::string get(const std::string& url, std::string_view method = "GET", bool close = true,
                       std::string_view version = "HTTP/1.1", std::string_view extra = {}) {
    return std::string(method) + " " + url + " " + std::string(version) + "\r\nHost: client-supplied.test\r\n" +
           std::string(extra) + (close ? "Connection: close\r\n" : "") + "\r\n";
}

inline http::HttpSessionConfig session_config(std::shared_ptr<proxy::ProxyStats> stats,
                                              proxy::ProxyConfig config = {}, std::size_t window = 256) {
    http::HttpSessionConfig session;
    session.max_output_bytes = window;
    session.keep_alive = true;
    session.application = test::take(proxy::make_forward_factory(std::move(config), std::move(stats)));
    return session;
}

struct Parsed { http::Response response; std::size_t consumed{}; bool complete{}; };
// Parses consecutive downstream responses; `methods` gives each originating method.
inline std::vector<Parsed> parse_all(std::string_view wire, std::vector<http::Method> methods) {
    std::vector<Parsed> result;
    for (const auto method : methods) {
        http::ResponseParser parser(method);
        auto consumed = parser.consume(wire);
        if (consumed.event != http::ParseEvent::complete && parser.framing() == http::BodyFraming::close_delimited) {
            (void)parser.finish_on_eof();
        }
        result.push_back({parser.response(), consumed.consumed, parser.complete()});
        wire.remove_prefix(consumed.consumed);
    }
    return result;
}
} // namespace proxy_test
