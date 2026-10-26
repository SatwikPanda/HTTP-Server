#include "event_loop_harness.hpp"
#include "fixtures/session_probe.hpp"

namespace {
using namespace std::chrono_literals;
using namespace concurrency;
using namespace concurrency::event_loop;
using namespace event_loop_test;

void fairness_budgets_requeue_large_transfer_beside_small_requests() {
    test::TemporaryDirectory dir;
    const std::string large_payload(256 * 1024, 'B');
    const std::string small_payload = "small-fast-response";
    (void)dir.write("large.bin", large_payload);
    const auto root = dir.write("small.txt", small_payload).parent_path();

    server::StaticConfig scfg;
    scfg.document_root = root;
    scfg.max_chunk_bytes = 4096;
    auto static_factory = test::take(server::make_static_factory(scfg));

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 128);

    http::HttpSessionConfig hcfg;
    hcfg.application = static_factory;
    hcfg.keep_alive = true;
    hcfg.max_output_bytes = 8192;

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    // Tight per-session budgets to force multi-turn requeueing of large transfers
    cfg.budgets.max_accepts_per_turn = 2;
    cfg.budgets.max_completions_per_turn = 4;
    cfg.budgets.max_sessions_per_turn = 2;
    cfg.budgets.max_events_per_session_turn = 2;
    cfg.budgets.max_bytes_per_session_turn = 2048;

    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    // Start large transfer on client 1 and read in background thread
    LoopbackClient large_client(ep);
    std::string large_received;
    std::thread reader([&] {
        large_client.write_all("GET /large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        large_received = large_client.read_http_response(http::Method::GET).body;
    });

    // While large transfer is running, issue 8 sequential small requests across separate clients
    for (int i = 0; i < 8; ++i) {
        LoopbackClient small_client(ep);
        small_client.write_all("GET /small.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto resp = small_client.read_http_response(http::Method::GET);
        CHECK(resp.status_code == 200 && resp.body == small_payload);
    }

    reader.join();
    CHECK(large_received == large_payload);

    driver->request_stop(500ms);
    driver->join();

    const auto st = driver->stats();
    CHECK(st.budget_requeues > 0);
    CHECK(st.peak_session_output_bytes <= hcfg.max_output_bytes);
    CHECK(st.active_sessions == 0);
}

void session_admission_overload_and_recovery() {
    http::HttpSessionConfig hcfg;
    hcfg.keep_alive = true;
    hcfg.response_body = "capacity-ok";

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    cfg.limits.max_sessions = 2;
    cfg.send_overload_503 = true;

    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        }
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    // Occupy both session slots
    LoopbackClient c1(ep);
    LoopbackClient c2(ep);
    c1.write_all("GET /1 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    c2.write_all("GET /2 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(c1.read_http_response().status_code == 200);
    CHECK(c2.read_http_response().status_code == 200);
    CHECK(driver->stats().active_sessions == 2);
    CHECK(driver->stats().peak_active_sessions == 2);

    // Close one client -> listener re-enables read interest and admits waiting client
    c1.close();
    const auto deadline = test::Clock::now() + 2s;
    while (driver->stats().active_sessions >= 2 && test::Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(driver->stats().active_sessions == 1);

    LoopbackClient c3(ep);
    c3.write_all("GET /3 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    auto r3 = c3.read_http_response();
    CHECK(r3.status_code == 200 && r3.body == "capacity-ok");
    c3.close();
    c2.close();

    driver->request_stop(500ms);
    driver->join();
    CHECK(driver->stats().active_sessions == 0);
}

void helper_queue_saturation_and_recovery() {
    test::TemporaryDirectory dir;
    const auto path = dir.write("item.txt", "helper-item");
    auto file_factory = http::make_file_reference_factory(path);

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);
    helpers->set_reject_submissions(true); // Simulate saturated helper queue

    core::MetricsSink* metrics_ptr = nullptr;
    struct LocalMetrics final : core::MetricsSink {
        std::atomic<std::uint64_t> rejected{0};
        void record(core::Metric m, std::uint64_t amount) noexcept override {
            if (m == core::Metric::work_rejected) rejected.fetch_add(amount, std::memory_order_relaxed);
        }
    } metrics;
    metrics_ptr = &metrics;

    http::HttpSessionConfig hcfg;
    hcfg.application = file_factory;
    hcfg.metrics = metrics_ptr;

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    cfg.metrics = metrics_ptr;

    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    // When helper queue rejects admission, transaction fails cleanly before commit (502) without spinning
    {
        LoopbackClient c1(ep);
        c1.write_all("GET /item.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto r1 = c1.read_http_response();
        CHECK(r1.status_code >= 500);
        CHECK(metrics.rejected.load() >= 1);
    }

    // Re-enable helper admission -> subsequent request succeeds
    helpers->set_reject_submissions(false);
    {
        LoopbackClient c2(ep);
        c2.write_all("GET /item.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto r2 = c2.read_http_response();
        CHECK(r2.status_code == 200 && r2.body == "helper-item");
    }

    driver->request_stop(500ms);
    driver->join();
    CHECK(driver->stats().pending_jobs == 0);
}

void slow_reader_output_progress_timeout() {
    test::TemporaryDirectory dir;
    // A 512 KiB response can fit entirely in POSIX socket buffers. Use a bounded
    // larger stream and verify actual would-block I/O before asserting timeout.
    const std::string big(16 * 1024 * 1024, 'Z');
    const auto path = dir.write("big.bin", big);
    auto file_factory = http::make_file_reference_factory(path);
    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);
    test::SessionProbe probe;

    http::HttpSessionConfig hcfg;
    hcfg.application = file_factory;
    hcfg.max_output_bytes = 4096;
    hcfg.output_progress_timeout = 120ms;
    hcfg.retry_timeout = 5000ms; // Only stalled output should reach the short deadline.

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg, &probe](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<test::ProbedSession>(
                std::make_unique<http::HttpSession>(hcfg, id), probe));
        },
        helpers
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    // Never read: the real socket eventually reports would-block to HttpSession.
    LoopbackClient stalled_client(ep);
    stalled_client.write_all("GET /big.bin HTTP/1.1\r\nHost: localhost\r\n\r\n");

    const auto deadline = test::Clock::now() + 3s;
    while (!probe.timed_out && test::Clock::now() < deadline) {
        std::this_thread::sleep_for(15ms);
    }
    CHECK(probe.blocked_writes > 0);
    CHECK(probe.bytes_written > 0 && probe.bytes_written < big.size());
    CHECK(probe.timed_out);
    CHECK(driver->stats().timeouts_fired >= 1);
    while (driver->stats().active_sessions != 0 && test::Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(driver->stats().active_sessions == 0);
    stalled_client.close();

    driver->request_stop(500ms);
    driver->join();
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    fairness_budgets_requeue_large_transfer_beside_small_requests();
    session_admission_overload_and_recovery();
    helper_queue_saturation_and_recovery();
    slow_reader_output_progress_timeout();
    return 0;
}
