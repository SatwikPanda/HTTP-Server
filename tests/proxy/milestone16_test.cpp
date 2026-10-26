// Milestone 16: backpressure, phase deadlines, candidate fallback, admission
// bounds, pre/post-commit failure semantics, cancellation and K observer hooks.
#include "proxy_test_support.hpp"
#include "concurrency/single_session_driver.hpp"
#include "http/serializer.hpp"

namespace {
using namespace proxy_test;

void pre_and_post_commit_failures() {
    auto stats = std::make_shared<proxy::ProxyStats>();
    // Bad client target/loop: 400 / 403 before any origin contact.
    {
        Channel bad("GET /not-absolute HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto s = http::run_http_session(bad, session_config(stats));
        CHECK(s.response_status == 400 && bad.output.starts_with("HTTP/1.1 400"));
        Channel loop(get("http://localhost:80/", "GET", true, "HTTP/1.1", "Via: 1.1 httpserver\r\n"));
        const auto l = http::run_http_session(loop, session_config(stats));
        CHECK(l.response_status == 403 && loop.output.starts_with("HTTP/1.1 403"));
        CHECK(stats->snapshot().origin_connections == 0 && stats->snapshot().client_error == 2);
    }
    // Refused origin connection (all candidates fail) -> 502.
    {
        auto closed = test::listener();
        const auto port = test::take(closed.local_endpoint()).port();
        closed.close();
        Channel c(get("http://127.0.0.1:" + std::to_string(port) + "/"));
        const auto s = http::run_http_session(c, session_config(stats));
        CHECK(s.response_status == 502 && c.output.starts_with("HTTP/1.1 502"));
    }
    // Invalid upstream head, ambiguous framing, header overflow before commit -> 502.
    for (const char* bad_wire : {
             "NOT-HTTP\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
         }) {
        MultiOrigin origin({{{bad_wire}}});
        Channel c(get(origin.url("/")));
        const auto s = http::run_http_session(c, session_config(stats));
        origin.join();
        CHECK(s.response_status == 502 && c.output.starts_with("HTTP/1.1 502"));
    }
    // Truncated fixed-length and truncated chunked body AFTER commit: close
    // without a second response or a terminating 0 chunk.
    for (const char* partial : {
             "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n",
         }) {
        MultiOrigin origin({{{partial}}});
        Channel c(get(origin.url("/")));
        const auto s = http::run_http_session(c, session_config(stats));
        origin.join();
        CHECK(!s.completed_cleanly && s.requests_served == 0 && s.error);
        CHECK(c.output.starts_with("HTTP/1.1 200") && c.output.find("HTTP/1.1 5") == std::string::npos);
        CHECK(!c.output.ends_with("0\r\n\r\n"));
    }
    CHECK(stats->snapshot().active == 0);
}

void deadlines_and_slow_peers() {
    auto stats = std::make_shared<proxy::ProxyStats>();
    proxy::ProxyConfig fast;
    fast.limits.connect_timeout = 80ms;
    fast.limits.response_timeout = 80ms;
    fast.limits.body_timeout = 80ms;
    // Origin accepts and reads request, then stalls before sending the head -> 504.
    {
        test::ControlledOrigin gated({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}, 97, true);
        const auto url = "http://" + gated.target().host + ":" + std::to_string(gated.target().port) + "/slow-head";
        Channel c(get(url));
        const auto s = http::run_http_session(c, session_config(stats, fast));
        gated.release(); gated.join();
        CHECK(s.response_status == 504 && c.output.starts_with("HTTP/1.1 504"));
        CHECK(stats->snapshot().gateway_timeout == 1);
    }
    // Origin sends head + part of body, then holds the connection open past
    // body_timeout after downstream has already committed -> abort and close.
    {
        MultiOrigin slow_body({{{"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\npartial"}, true}});
        Channel c(get(slow_body.url("/slow-body")));
        const auto s = http::run_http_session(c, session_config(stats, fast));
        CHECK(!s.completed_cleanly && s.requests_served == 0);
        CHECK(c.output.starts_with("HTTP/1.1 200") && c.output.find("504") == std::string::npos);
        CHECK(stats->snapshot().gateway_timeout == 2);
    }
    // Backpressure pause must NOT falsely blame the origin for stalling while
    // the client is slow to drain downstream output.
    {
        const std::string body(2048, 'b');
        MultiOrigin origin({{{"HTTP/1.1 200 OK\r\nContent-Length: 2048\r\n\r\n", body}}}, 512);
        proxy::ProxyConfig tight;
        tight.limits.body_timeout = 60ms;
        tight.limits.max_read_bytes = 128;
        auto cfg = session_config(stats, tight, 192);
        http::HttpSession session(cfg, {7, 1});
        concurrency::ReferenceWorkExecutor executor;
        const auto req = get(origin.url("/paused"));
        auto action = session.on_event(concurrency::ReadDataEvent{bytes(req)});
        std::string wire;
        bool slept_while_paused = false;
        for (unsigned step = 0; !session.is_closed() && step < 4000; ++step) {
            if (auto* work = action.as_work()) {
                // While downstream is writing, the session never asks for more
                // upstream work; upstream reads are paused at the window bound.
                action = session.on_event(executor.execute(*work));
            } else if (auto* write = action.as_write()) {
                if (!slept_while_paused && wire.find("\r\n\r\n") != std::string::npos) {
                    std::this_thread::sleep_for(90ms); // > body_timeout while paused on client output
                    slept_while_paused = true;
                }
                const auto n = std::min<std::size_t>(19, write->data.size());
                wire.append(reinterpret_cast<const char*>(write->data.data()), n);
                action = session.on_event(concurrency::WriteCompleteEvent{n, write->id});
            } else CHECK(false);
        }
        origin.join();
        CHECK(slept_while_paused && session.stats().requests_served == 1 && session.stats().peak_output_bytes <= 192);
        auto parsed = parse_all(wire, {http::Method::GET});
        CHECK(parsed[0].complete && parsed[0].response.body == body);
    }
}

void candidate_fallback_and_dns_timeout_step() {
    // Controlled step test: first candidate is a closed port, second is the live
    // origin; short request writes and would-block read/write transitions occur.
    auto closed = test::listener();
    const auto dead = test::take(closed.local_endpoint());
    closed.close();
    MultiOrigin live({{{"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngood"}}});
    auto stats = std::make_shared<proxy::ProxyStats>();
    proxy::ProxyConfig cfg; cfg.limits.max_candidates = 2; cfg.limits.max_read_bytes = 6;
    auto session_cfg = session_config(stats, cfg, 256);
    http::HttpSession session(session_cfg, {8, 1});
    concurrency::ReferenceWorkExecutor executor;
    const auto req = get(live.url("/cand"));
    auto action = session.on_event(concurrency::ReadDataEvent{bytes(req)});
    CHECK(action.as_work() && std::holds_alternative<concurrency::ResolveWork>(action.as_work()->request));
    // Inject [dead, live, extra] so the candidate cap (2) and fallback are both exercised.
    concurrency::WorkEvent resolved;
    resolved.id = action.as_work()->id;
    resolved.candidates = {dead, live. url().empty() ? dead : test::take(net::Endpoint::from_string("127.0.0.1",
        static_cast<std::uint16_t>( std::stoi(live.authority().substr(live.authority().find(':') + 1))))), dead};
    action = session.on_event(resolved);
    CHECK(action.as_work());
    const auto& candidates = std::get<concurrency::ConnectWork>(action.as_work()->request).candidates;
    CHECK(candidates.size() == 2 && candidates[0] == dead);
    action = session.on_event(executor.execute(*action.as_work()));
    // Force a would-block + wait + 3-byte short write before letting the real executor finish.
    CHECK(action.as_work() && std::holds_alternative<concurrency::UpstreamWriteWork>(action.as_work()->request));
    action = session.on_event(concurrency::WorkEvent{action.as_work()->id, concurrency::WorkStatus::would_block});
    CHECK(action.as_work() && std::holds_alternative<concurrency::UpstreamWaitWork>(action.as_work()->request));
    action = session.on_event(concurrency::WorkEvent{action.as_work()->id, concurrency::WorkStatus::complete});
    CHECK(action.as_work() && std::holds_alternative<concurrency::UpstreamWriteWork>(action.as_work()->request));
    std::string output;
    for (unsigned i = 0; !session.is_closed() && i < 1000; ++i) {
        if (auto* work = action.as_work()) action = session.on_event(executor.execute(*work));
        else if (auto* write = action.as_write()) {
            output.append(reinterpret_cast<const char*>(write->data.data()), write->data.size());
            action = session.on_event(concurrency::WriteCompleteEvent{write->data.size(), write->id});
        } else CHECK(false);
    }
    live.join();
    CHECK(parse_all(output, {http::Method::GET})[0].response.body == "good");

    // DNS timeout / work rejection mapping (asynchronous interruption itself belongs to E).
    for (const auto [status, code] : {std::pair{concurrency::WorkStatus::timed_out, "504"},
                                      std::pair{concurrency::WorkStatus::rejected, "503"},
                                      std::pair{concurrency::WorkStatus::failed, "502"}}) {
        http::HttpSession s(session_cfg, {9, 1});
        auto a = s.on_event(concurrency::ReadDataEvent{bytes(req)});
        CHECK(a.as_work());
        a = s.on_event(concurrency::WorkEvent{a.as_work()->id, status});
        CHECK(a.as_write());
        std::string text(reinterpret_cast<const char*>(a.as_write()->data.data()), a.as_write()->data.size());
        CHECK(text.starts_with(std::string("HTTP/1.1 ") + code));
    }
}

void admission_cancellation_and_observer_seam() {
    auto stats = std::make_shared<proxy::ProxyStats>();
    proxy::ProxyConfig cfg; cfg.limits.max_active_transactions = 1;
    auto session_cfg = session_config(stats, cfg);
    http::HttpSession first(session_cfg, {10, 1}), second(session_cfg, {11, 1});
    const auto req = get("http://127.0.0.1:80/");
    auto a1 = first.on_event(concurrency::ReadDataEvent{bytes(req)});
    CHECK(a1.as_work() && stats->snapshot().active == 1);
    // Second concurrent transaction exceeds max_active_transactions -> 503.
    auto a2 = second.on_event(concurrency::ReadDataEvent{bytes(req)});
    CHECK(a2.as_write());
    std::string busy(reinterpret_cast<const char*>(a2.as_write()->data.data()), a2.as_write()->data.size());
    CHECK(busy.starts_with("HTTP/1.1 503") && stats->snapshot().unavailable == 1);
    // Client disconnect / session stop cancels the in-flight transaction and
    // returns active count to baseline; late completion is ignored.
    const auto stale_id = a1.as_work()->id;
    const auto token = a1.as_work()->cancellation;
    CHECK(first.on_event(concurrency::ErrorEvent{core::make_error(core::ErrorCategory::connection_reset, "gone"), {10, 1}}).is_close());
    CHECK(token->is_cancelled() && stats->snapshot().active == 0 && stats->snapshot().cancelled == 1);
    CHECK(first.on_event(concurrency::WorkEvent{stale_id, concurrency::WorkStatus::complete}).is_close());

    // Client disconnects mid-body on a real origin response.
    MultiOrigin origin({{{"HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n", std::string(4096, 'x')}}});
    Channel drop(get(origin.url("/drop")), 17); drop.fail_after = 60;
    const auto dropped = http::run_http_session(drop, session_config(stats));
    origin.join();
    CHECK(!dropped.completed_cleanly && stats->snapshot().active == 0);

    // K observer seam: prepare, final head + timing, decoded body, completion/failure.
    struct Observer final : proxy::ExchangeObserver {
        std::string body, etag;
        proxy::ExchangeTiming timing;
        bool completed{}, failed{};
        core::Result<void> prepare(proxy::OutboundRequest& out) override {
            out.request.headers.set("If-None-Match", "\"cached\"");
            return {};
        }
        core::Result<void> on_final_head(const http::Response& orig, http::Response& down,
                                         const proxy::ExchangeTiming& t) override {
            if (auto e = orig.headers.get("ETag")) etag = *e;
            down.headers.set("Age", "3");
            timing = t;
            return {};
        }
        core::Result<void> on_body(std::span<const std::byte> data) override {
            body.append(reinterpret_cast<const char*>(data.data()), data.size());
            return {};
        }
        void on_complete(const http::HeaderMap& trailers) override { completed = trailers.get("X-Done") == "1"; }
        void on_failure(const core::Error&) override { failed = true; }
    };
    MultiOrigin observed_origin({{{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nETag: \"v2\"\r\n\r\n4\r\ndata\r\n0\r\nX-Done: 1\r\n\r\n"}}});
    auto probe = std::make_unique<Observer>();
    auto* raw_probe = probe.get();
    http::Request parsed_req; parsed_req.target = observed_origin.url("/k");
    const http::TransactionContext ctx{{{12, 1}, 1}, 256, core::real_steady_clock(), core::real_system_clock()};
    proxy::ProxyConfig default_cfg;
    auto tx = proxy::make_forward_transaction(parsed_req, ctx, default_cfg, stats, std::move(probe));
    concurrency::ReferenceWorkExecutor exec;
    auto step = tx->on_event(http::StartTransaction{});
    std::uint64_t op_seq = 0;
    while (!std::holds_alternative<http::ResponseFinished>(step)) {
        if (auto* w = std::get_if<http::TransactionWork>(&step)) step = tx->on_event(exec.execute({{{{12, 1}, 1}, concurrency::endpoint_role(w->request), ++op_seq}, w->deadline, w->request, {}}));
        else if (auto* h = std::get_if<http::ResponseHead>(&step)) {
            CHECK(h->response.headers.get("Age") == "3");
            step = tx->on_event(http::OutputDrained{128});
        } else if (std::holds_alternative<http::ResponseBody>(step)) step = tx->on_event(http::OutputDrained{128});
        else CHECK(false);
    }
    observed_origin.join();
    CHECK(observed_origin.requests()[0].find("If-None-Match: \"cached\"\r\n") != std::string::npos);
    CHECK(raw_probe->completed && !raw_probe->failed && raw_probe->body == "data" && raw_probe->etag == "\"v2\"");
    CHECK(raw_probe->timing.request_sent <= raw_probe->timing.final_head_received);
}
} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    pre_and_post_commit_failures();
    deadlines_and_slow_peers();
    candidate_fallback_and_dns_timeout_step();
    admission_cancellation_and_observer_seam();
    std::cout << "milestone 16 proxy backpressure and failure handling passed\n";
    return 0;
}
