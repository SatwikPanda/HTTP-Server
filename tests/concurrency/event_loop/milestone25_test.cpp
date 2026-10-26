#include "event_loop_harness.hpp"

namespace {
using namespace std::chrono_literals;
using namespace concurrency;
using namespace concurrency::event_loop;
using namespace event_loop_test;

void validate_config_and_factory() {
    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    CHECK(validate_event_loop_config(cfg));

    auto bad = cfg;
    bad.clock = nullptr;
    CHECK(!validate_event_loop_config(bad));

    bad = cfg;
    bad.metrics = nullptr;
    CHECK(!validate_event_loop_config(bad));

    bad = cfg;
    bad.budgets.max_accepts_per_turn = 0;
    CHECK(!validate_event_loop_config(bad));

    bad = cfg;
    bad.limits.max_connecting = bad.limits.max_upstreams + 1;
    CHECK(!validate_event_loop_config(bad));

    CHECK(!make_event_loop_driver(cfg, SessionFactory{}));
}

void echo_and_demo_sessions_with_idle_clients() {
    // 1. Echo sessions with multiple concurrent idle clients and active transfers
    EventLoopConfig echo_cfg;
    echo_cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto echo_driver = test::take(make_event_loop_driver(
        echo_cfg,
        [](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(
                std::make_unique<EchoSession>(id, 4096, 4096, 5000ms)
            );
        }
    ));
    CHECK(echo_driver->start());
    const auto echo_ep = echo_driver->local_endpoint();

    std::vector<std::unique_ptr<LoopbackClient>> idle_clients;
    for (int i = 0; i < 12; ++i) {
        idle_clients.push_back(std::make_unique<LoopbackClient>(echo_ep));
    }

    LoopbackClient active1(echo_ep);
    LoopbackClient active2(echo_ep);
    const std::string payload1 = "Echo-Payload-One-1234567890";
    const std::string payload2(16384, 'E');
    active1.write_all(payload1);
    active2.write_all(payload2);
    CHECK(active1.read_exact(payload1.size()) == payload1);
    CHECK(active2.read_exact(payload2.size()) == payload2);

    active1.close();
    active2.close();
    for (auto& c : idle_clients) c->close();

    echo_driver->request_stop(500ms);
    echo_driver->join();
    const auto echo_stats = echo_driver->stats();
    CHECK(echo_stats.connections_accepted == 14);
    CHECK(echo_stats.active_sessions == 0);
}

void static_file_serving_and_persistent_pipelines() {
    test::TemporaryDirectory dir;
    const std::string index_body = "<html><body>Hello Event Loop</body></html>";
    const std::string large_body(128 * 1024, 'S');
    (void)dir.write("index.html", index_body);
    const auto root = dir.write("large.bin", large_body).parent_path();

    server::StaticConfig static_cfg;
    static_cfg.document_root = root;
    static_cfg.max_chunk_bytes = 8192;
    auto static_factory = test::take(server::make_static_factory(static_cfg));

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 128);
    const auto loop_caller_tid = std::this_thread::get_id();

    http::HttpSessionConfig http_cfg;
    http_cfg.application = static_factory;
    http_cfg.keep_alive = true;
    http_cfg.max_output_bytes = 16384;
    http_cfg.max_requests_per_connection = 10;

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [http_cfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(http_cfg, id));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    // Coexist with 16 idle clients
    std::vector<std::unique_ptr<LoopbackClient>> idle;
    for (int i = 0; i < 16; ++i) {
        idle.push_back(std::make_unique<LoopbackClient>(ep));
    }

    // Persistent + pipelined GET, HEAD, conditional 304, 404, traversal 403, large GET
    LoopbackClient client(ep);
    const std::string pipeline =
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /missing.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /../secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n";
    client.write_all(pipeline);

    auto r1 = client.read_http_response(http::Method::GET);
    CHECK(r1.status_code == 200 && r1.body == index_body);
    const auto etag = r1.headers.get("ETag");
    CHECK(etag.has_value());

    auto r2 = client.read_http_response(http::Method::HEAD);
    CHECK(r2.status_code == 200 && r2.body.empty());
    CHECK(r2.headers.get("Content-Length") == std::to_string(large_body.size()));

    auto r3 = client.read_http_response(http::Method::GET);
    CHECK(r3.status_code == 404);

    auto r4 = client.read_http_response(http::Method::GET);
    CHECK(r4.status_code == 403);

    auto r5 = client.read_http_response(http::Method::GET);
    CHECK(r5.status_code == 200 && r5.body == large_body);

    // Conditional request matching ETag -> 304 Not Modified
    client.write_all(
        "GET / HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: " + std::string(*etag) +
        "\r\nConnection: close\r\n\r\n"
    );
    auto r6 = client.read_http_response(http::Method::GET);
    CHECK(r6.status_code == 304 && r6.body.empty());

    // Confirm file jobs executed on helper worker threads, not the caller
    CHECK(helpers->submitted_count() > 0);
    CHECK(helpers->completed_count() == helpers->submitted_count());
    CHECK(helpers->last_worker_thread() != std::thread::id{} && helpers->last_worker_thread() != loop_caller_tid);

    client.close();
    for (auto& c : idle) c->close();

    driver->request_stop(500ms);
    driver->join();
    const auto st = driver->stats();
    CHECK(st.active_sessions == 0 && st.pending_jobs == 0);
    CHECK(st.peak_session_output_bytes <= http_cfg.max_output_bytes);
}

void delayed_file_jobs_do_not_stall_buffered_work_or_timers() {
    test::TemporaryDirectory dir;
    const auto file_path = dir.write("slow.txt", "delayed-file-contents");

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(1, 64);
    helpers->pause_execution(true); // Pause file helper execution

    // Session 1 uses file reference factory; Session 2 uses buffered demo factory;
    // Session 3 has a short 80ms idle timeout.
    std::atomic<int> session_seq{0};
    auto file_factory = http::make_file_reference_factory(file_path);

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [&](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            const int idx = ++session_seq;
            http::HttpSessionConfig hcfg;
            hcfg.keep_alive = false;
            if (idx == 1) {
                hcfg.application = file_factory;
                hcfg.retry_timeout = 5000ms;
            } else if (idx == 2) {
                hcfg.response_body = "fast-buffered-ok";
                hcfg.retry_timeout = 5000ms;
            } else {
                hcfg.idle_timeout = 80ms;
                hcfg.retry_timeout = 80ms;
            }
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    LoopbackClient slow_file_client(ep);
    slow_file_client.write_all("GET /slow.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");

    // Wait briefly until the file job is submitted and paused
    const auto wait_deadline = test::Clock::now() + 2s;
    while (helpers->submitted_count() == 0 && test::Clock::now() < wait_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(helpers->submitted_count() >= 1);

    // While file job is paused, buffered session 2 must complete immediately
    LoopbackClient fast_client(ep);
    fast_client.write_all("GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto fast_resp = fast_client.read_http_response(http::Method::GET);
    CHECK(fast_resp.status_code == 200 && fast_resp.body == "fast-buffered-ok");
    fast_client.close();

    // Session 3 (idle client with 80ms timeout) must time out while file job is still paused
    LoopbackClient idle_timeout_client(ep);
    const auto t_deadline = test::Clock::now() + 2s;
    while (driver->stats().timeouts_fired == 0 && test::Clock::now() < t_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(driver->stats().timeouts_fired >= 1);
    idle_timeout_client.close();

    // Unpause helper and verify the delayed file request completes cleanly
    helpers->pause_execution(false);
    auto file_resp = slow_file_client.read_http_response(http::Method::GET);
    CHECK(file_resp.status_code == 200 && file_resp.body == "delayed-file-contents");
    slow_file_client.close();

    driver->request_stop(500ms);
    driver->join();
}

void readable_with_hangup_and_writable_interest_removal() {
    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);

    http::HttpSessionConfig hcfg;
    hcfg.keep_alive = true;
    hcfg.idle_timeout = 2000ms;
    hcfg.response_body = "hangup-and-interest-ok";

    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        }
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    // 1. Send request and immediately half-close (FIN) so readable + hangup arrive together
    {
        LoopbackClient client(ep);
        client.write_all("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        client.shutdown_write();
        auto resp = client.read_http_response(http::Method::GET);
        CHECK(resp.status_code == 200 && resp.body == "hangup-and-interest-ok");
    }

    // 2. Keep-alive session: after response is sent, writable interest must be removed
    // so the event loop does not spin while the client stays idle.
    {
        LoopbackClient keepalive_client(ep);
        keepalive_client.write_all("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        auto resp = keepalive_client.read_http_response(http::Method::GET);
        CHECK(resp.status_code == 200 && resp.body == "hangup-and-interest-ok");

        const auto before = driver->stats().iterations;
        std::this_thread::sleep_for(60ms);
        const auto after = driver->stats().iterations;
        // Without writable-interest removal, a writable socket spins thousands of iterations.
        CHECK(after - before <= 5);
        keepalive_client.close();
    }

    driver->request_stop(500ms);
    driver->join();
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    validate_config_and_factory();
    echo_and_demo_sessions_with_idle_clients();
    static_file_serving_and_persistent_pipelines();
    delayed_file_jobs_do_not_stall_buffered_work_or_timers();
    readable_with_hangup_and_writable_interest_removal();
    return 0;
}
