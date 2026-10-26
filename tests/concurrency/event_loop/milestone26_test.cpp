#include "event_loop_harness.hpp"

namespace {
using namespace std::chrono_literals;
using namespace concurrency;
using namespace concurrency::event_loop;
using namespace event_loop_test;

// Custom transaction that connects through a candidate list (first refused, second live)
// to verify nonblocking candidate fallback with fresh sockets on the event loop.
class FallbackOriginTransaction final : public http::Transaction {
public:
    FallbackOriginTransaction(std::vector<net::Endpoint> candidates, net::HostPort authority, const http::TransactionContext& ctx)
        : candidates_(std::move(candidates)), parser_(http::Method::GET),
          deadline_(ctx.steady_clock.now() + 5s),
          capacity_(std::min<std::size_t>(ctx.response_window - 20, 16384)) {
        http::Request req;
        req.method = http::Method::GET;
        req.target = "/fallback";
        req.headers.set("Host", authority.to_string());
        req.headers.set("Connection", "close");
        const auto wire = test::take(http::serialize_request_head(req));
        const auto raw = as_bytes(wire);
        request_ = std::make_shared<const Bytes>(raw.begin(), raw.end());
    }

    http::TransactionStep on_event(const http::TransactionEvent& event) override {
        if (std::holds_alternative<http::StartTransaction>(event)) {
            return http::TransactionWork{ConnectWork{candidates_}, deadline_};
        }
        if (const auto* drained = std::get_if<http::OutputDrained>(&event)) {
            capacity_ = std::min<std::size_t>(drained->body_capacity, 16384);
            return pump();
        }
        const auto& res = std::get<WorkEvent>(event);
        if (res.status == WorkStatus::failed || res.status == WorkStatus::rejected) {
            return http::TransactionFailure{res.error, http::StatusCode::BadGateway};
        }
        if (res.status == WorkStatus::timed_out || res.status == WorkStatus::cancelled) {
            return http::TransactionFailure{res.error, http::StatusCode::GatewayTimeout};
        }
        if (state_ == State::connect) {
            state_ = State::write;
            return http::TransactionWork{UpstreamWriteWork{request_, 0, request_->size()}, deadline_};
        }
        if (state_ == State::wait_write) {
            state_ = State::write;
            return http::TransactionWork{UpstreamWriteWork{request_, sent_, request_->size() - sent_}, deadline_};
        }
        if (state_ == State::write) {
            if (res.status == WorkStatus::would_block) {
                state_ = State::wait_write;
                return http::TransactionWork{UpstreamWaitWork{net::Interest::write}, deadline_};
            }
            sent_ += res.transferred;
            if (sent_ < request_->size()) {
                return http::TransactionWork{UpstreamWriteWork{request_, sent_, request_->size() - sent_}, deadline_};
            }
            state_ = State::read;
            return http::TransactionWork{UpstreamReadWork{capacity_}, deadline_};
        }
        if (state_ == State::wait_read) {
            state_ = State::read;
            return http::TransactionWork{UpstreamReadWork{capacity_}, deadline_};
        }
        if (state_ == State::closing) {
            return http::ResponseFinished{};
        }
        if (res.status == WorkStatus::would_block) {
            state_ = State::wait_read;
            return http::TransactionWork{UpstreamWaitWork{net::Interest::read}, deadline_};
        }
        if (res.status == WorkStatus::eof) {
            if (!parser_.finish_on_eof()) {
                return http::TransactionFailure{parser_.error_detail(), http::StatusCode::BadGateway};
            }
        } else if (res.status == WorkStatus::progress && res.bytes) {
            input_ = res.bytes;
            consumed_ = 0;
        }
        return pump();
    }

private:
    enum class State { connect, write, wait_write, read, wait_read, closing };

    http::TransactionStep pump() {
        for (;;) {
            const auto span = input_ ? std::span(*input_).subspan(consumed_) : std::span<const std::byte>{};
            const auto ev = parser_.consume_stream(span, capacity_);
            consumed_ += ev.consumed;
            if (ev.kind == http::ResponseEventKind::informational_head || ev.kind == http::ResponseEventKind::trailers) continue;
            if (ev.kind == http::ResponseEventKind::error) {
                return http::TransactionFailure{parser_.error_detail(), http::StatusCode::BadGateway};
            }
            if (ev.kind == http::ResponseEventKind::need_more) {
                input_.reset();
                consumed_ = 0;
                return http::TransactionWork{UpstreamReadWork{capacity_}, deadline_};
            }
            if (ev.kind == http::ResponseEventKind::complete) {
                state_ = State::closing;
                return http::TransactionWork{UpstreamCloseWork{}, deadline_};
            }
            if (ev.kind == http::ResponseEventKind::body) {
                return http::ResponseBody{std::make_shared<const Bytes>(ev.body.begin(), ev.body.end())};
            }
            if (ev.kind == http::ResponseEventKind::final_head) {
                http::Response head(parser_.response().status_code, parser_.response().reason);
                head.headers.set("Content-Type", "text/plain");
                return http::ResponseHead{std::move(head), http::BodyFraming::chunked, {}};
            }
        }
    }

    std::vector<net::Endpoint> candidates_;
    SharedBytes request_;
    SharedBytes input_;
    http::ResponseParser parser_;
    core::SteadyTimePoint deadline_;
    std::size_t capacity_{}, sent_{}, consumed_{};
    State state_{State::connect};
};

class FallbackOriginFactory final : public http::TransactionFactory {
public:
    FallbackOriginFactory(std::vector<net::Endpoint> candidates, net::HostPort authority)
        : candidates_(std::move(candidates)), authority_(std::move(authority)) {}

    core::Result<std::unique_ptr<http::Transaction>> create(
        const http::Request&, const http::TransactionContext& ctx
    ) const override {
        return std::unique_ptr<http::Transaction>(
            std::make_unique<FallbackOriginTransaction>(candidates_, authority_, ctx)
        );
    }

private:
    std::vector<net::Endpoint> candidates_;
    net::HostPort authority_;
};

void streaming_proxy_and_interim_chunked_trailers() {
    const std::string body(16384, 'P');
    const auto chunk = test::take(http::encode_chunk(body));
    test::ControlledOrigin origin({
        "HTTP/1.1 103 Early Hints\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n",
        chunk,
        "0\r\nX-Origin-Trailer: verified\r\n\r\n"
    }, 53);

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 128);
    http::HttpSessionConfig hcfg;
    hcfg.max_output_bytes = 512; // Small window to force downstream pause/resume backpressure
    hcfg.keep_alive = true;
    hcfg.application = http::make_origin_reference_factory(origin.target());

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());

    LoopbackClient client(driver->local_endpoint());
    client.write_all("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    auto resp = client.read_http_response(http::Method::GET);
    CHECK(resp.status_code == 200);
    CHECK(resp.body == body);
    CHECK(resp.headers.get("Content-Encoding") == "gzip");
    CHECK(resp.trailers.get("X-Origin-Trailer") == "verified");

    origin.join();
    CHECK(origin.request_count() == 1);

    driver->request_stop(500ms);
    driver->join();
    const auto st = driver->stats();
    CHECK(st.active_sessions == 0 && st.active_upstreams == 0 && st.active_connecting == 0);
    CHECK(st.peak_session_output_bytes <= hcfg.max_output_bytes);
}

void candidate_connect_fallback_and_refused_origin() {
    // Prepare a closed port for candidate 1 and a live origin for candidate 2
    auto temp_listener = test::listener();
    const auto refused_ep = test::take(temp_listener.local_endpoint());
    temp_listener.close();

    test::ControlledOrigin live_origin({"HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nfallback-ok"}, 64);
    CHECK(refused_ep != live_origin.endpoint());

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);
    http::HttpSessionConfig hcfg;
    hcfg.keep_alive = true;
    hcfg.application = std::make_shared<FallbackOriginFactory>(
        std::vector<net::Endpoint>{refused_ep, live_origin.endpoint()},
        live_origin.target()
    );

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());

    LoopbackClient client(driver->local_endpoint());
    client.write_all("GET /fallback HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto resp = client.read_http_response(http::Method::GET);
    CHECK(resp.status_code == 200 && resp.body == "fallback-ok");
    live_origin.join();

    // Now test when ALL candidates are refused -> pre-commit 502 Bad Gateway
    auto temp2 = test::listener();
    const auto refused2 = test::take(temp2.local_endpoint());
    temp2.close();

    http::HttpSessionConfig fail_hcfg;
    fail_hcfg.application = std::make_shared<FallbackOriginFactory>(
        std::vector<net::Endpoint>{refused_ep, refused2},
        net::HostPort{"127.0.0.1", refused_ep.port()}
    );
    EventLoopConfig fail_cfg;
    fail_cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto fail_driver = test::take(make_event_loop_driver(
        fail_cfg,
        [fail_hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(fail_hcfg, id));
        },
        helpers
    ));
    CHECK(fail_driver->start());

    LoopbackClient client2(fail_driver->local_endpoint());
    client2.write_all("GET /fail HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto fail_resp = client2.read_http_response(http::Method::GET);
    CHECK(fail_resp.status_code == 502);

    client.close();
    client2.close();
    driver->request_stop(500ms);
    driver->join();
    fail_driver->request_stop(500ms);
    fail_driver->join();
}

void post_commit_origin_truncation_aborts_without_final_chunk() {
    test::ControlledOrigin truncated({"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort"});
    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);

    http::HttpSessionConfig hcfg;
    hcfg.max_output_bytes = 256;
    hcfg.application = http::make_origin_reference_factory(truncated.target());

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());

    LoopbackClient client(driver->local_endpoint());
    client.write_all("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const std::string raw = client.read_until_eof();
    CHECK(raw.starts_with("HTTP/1.1 200 OK"));
    CHECK(raw.find("502") == std::string::npos);
    CHECK(!raw.ends_with("0\r\n\r\n"));

    truncated.join();
    driver->request_stop(500ms);
    driver->join();
}

void slow_origin_does_not_stall_unrelated_sessions() {
    test::ControlledOrigin gated_origin({"HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\ngated-origin"}, 64, true);
    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);

    std::atomic<int> session_seq{0};
    auto origin_factory = http::make_origin_reference_factory(gated_origin.target());

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [&](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            const int idx = ++session_seq;
            http::HttpSessionConfig hcfg;
            hcfg.keep_alive = true;
            if (idx == 1) {
                hcfg.application = origin_factory;
            } else {
                hcfg.response_body = "cached-or-buffered-hit";
            }
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    LoopbackClient proxy_client(ep);
    proxy_client.write_all("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    gated_origin.wait_for_request();

    // While proxy_client is waiting on the gated upstream socket, 5 fast clients complete
    for (int i = 0; i < 5; ++i) {
        LoopbackClient fast(ep);
        fast.write_all("GET /hit HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto r = fast.read_http_response(http::Method::GET);
        CHECK(r.status_code == 200 && r.body == "cached-or-buffered-hit");
    }

    // Release gated origin and verify proxy_client finishes
    gated_origin.release();
    auto proxy_resp = proxy_client.read_http_response(http::Method::GET);
    CHECK(proxy_resp.status_code == 200 && proxy_resp.body == "gated-origin");
    gated_origin.join();

    driver->request_stop(500ms);
    driver->join();
    const auto st = driver->stats();
    CHECK(st.active_sessions == 0 && st.active_upstreams == 0);
}

void upstream_admission_limit_enforced_and_recovers() {
    test::ControlledOrigin origin1({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none"}, 64, true);
    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);

    std::atomic<int> seq{0};
    auto f1 = http::make_origin_reference_factory(origin1.target());

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    cfg.limits.max_upstreams = 1;
    cfg.limits.max_connecting = 1;

    std::shared_ptr<const http::TransactionFactory> f2;
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [&](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            const int n = ++seq;
            http::HttpSessionConfig hcfg;
            hcfg.application = (n <= 2) ? f1 : f2;
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    LoopbackClient c1(ep);
    c1.write_all("GET /1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    origin1.wait_for_request();
    CHECK(driver->stats().active_upstreams == 1);

    // Second concurrent upstream connect exceeds max_upstreams=1 -> rejected -> 502
    LoopbackClient c2(ep);
    c2.write_all("GET /2 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    auto r2 = c2.read_http_response(http::Method::GET);
    CHECK(r2.status_code == 502);
    c2.close();

    // Release origin1; c1 completes and frees upstream slot
    origin1.release();
    auto r1 = c1.read_http_response(http::Method::GET);
    CHECK(r1.status_code == 200 && r1.body == "one");
    c1.close();
    origin1.join();

    // Wait for upstream gauge to return to 0, then third request succeeds using origin2
    const auto deadline = test::Clock::now() + 2s;
    while (driver->stats().active_upstreams != 0 && test::Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(driver->stats().active_upstreams == 0);

    test::ControlledOrigin origin2({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo"}, 64, false);
    f2 = http::make_origin_reference_factory(origin2.target());

    LoopbackClient c3(ep);
    c3.write_all("GET /3 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    auto r3 = c3.read_http_response(http::Method::GET);
    CHECK(r3.status_code == 200 && r3.body == "two");
    c3.close();
    origin2.join();

    driver->request_stop(500ms);
    driver->join();
}

void streaming_proxy_and_interim_chunked_trailers();
void candidate_connect_fallback_and_refused_origin();
void post_commit_origin_truncation_aborts_without_final_chunk();
void slow_origin_does_not_stall_unrelated_sessions();
void upstream_admission_limit_enforced_and_recovers();

void deterministic_single_step_and_reused_registration_batch() {
    http::HttpSessionConfig hcfg;
    hcfg.response_body = "step-once-ok";
    hcfg.keep_alive = false;
    hcfg.idle_timeout = 100ms;
    hcfg.retry_timeout = 100ms;

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    cfg.budgets.max_accepts_per_turn = 4;

    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        }
    ));
    CHECK(driver->bind_listener());
    const auto ep = driver->local_endpoint();

    // Run client 1 on a background thread while stepping the loop deterministically on main thread
    std::atomic<bool> c1_done{false};
    std::thread t1([&] {
        LoopbackClient c1(ep);
        c1.write_all("GET /step1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto r1 = c1.read_http_response();
        CHECK(r1.status_code == 200 && r1.body == "step-once-ok");
        c1.close();
        c1_done.store(true, std::memory_order_release);
    });

    while (driver->stats().connections_accepted == 0 || driver->stats().active_sessions != 0) {
        CHECK(test::take(driver->step_once()));
    }
    t1.join();
    CHECK(c1_done.load(std::memory_order_acquire));
    CHECK(driver->stats().connections_accepted == 1);
    CHECK(driver->stats().active_sessions == 0);

    // Client 2 reuses OS socket / poller slot
    std::atomic<bool> c2_done{false};
    std::thread t2([&] {
        LoopbackClient c2(ep);
        c2.write_all("GET /step2 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto r2 = c2.read_http_response();
        CHECK(r2.status_code == 200 && r2.body == "step-once-ok");
        c2.close();
        c2_done.store(true, std::memory_order_release);
    });

    while (driver->stats().connections_accepted < 2 || driver->stats().active_sessions != 0) {
        CHECK(test::take(driver->step_once()));
    }
    t2.join();
    CHECK(c2_done.load(std::memory_order_acquire));
    CHECK(driver->stats().connections_accepted == 2);
    CHECK(driver->stats().active_sessions == 0);

    driver->request_stop(0ms);
    (void)test::take(driver->step_once());
    CHECK(!driver->is_running());
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    streaming_proxy_and_interim_chunked_trailers();
    candidate_connect_fallback_and_refused_origin();
    post_commit_origin_truncation_aborts_without_final_chunk();
    slow_origin_does_not_stall_unrelated_sessions();
    upstream_admission_limit_enforced_and_recovers();
    deterministic_single_step_and_reused_registration_batch();
    return 0;
}
