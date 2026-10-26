#include "event_loop_harness.hpp"

namespace {
using namespace std::chrono_literals;
using namespace concurrency;
using namespace concurrency::event_loop;
using namespace event_loop_test;

void stop_during_idle_and_active_file_work() {
    test::TemporaryDirectory dir;
    const auto path = dir.write("data.txt", "shutdown-test-payload");
    auto file_factory = http::make_file_reference_factory(path);

    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);
    helpers->pause_execution(true);

    http::HttpSessionConfig hcfg;
    hcfg.application = file_factory;
    hcfg.keep_alive = true;

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
    const auto ep = driver->local_endpoint();

    LoopbackClient idle_client(ep);
    LoopbackClient file_client(ep);
    file_client.write_all("GET /data.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");

    const auto wait_deadline = test::Clock::now() + 2s;
    while (helpers->submitted_count() == 0 && test::Clock::now() < wait_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(helpers->submitted_count() >= 1);

    // Request stop while helper work is in-flight and idle client is connected.
    // Waiting work is cancelled immediately on stop; late helper completions arriving
    // after driver shutdown must not crash or touch destroyed poller/session state.
    driver->request_stop(100ms);
    driver->join();
    CHECK(!driver->is_running());

    // Now release the paused helper thread AFTER driver has already stopped and joined!
    helpers->pause_execution(false);
    while (helpers->completed_count() < helpers->submitted_count() && test::Clock::now() < wait_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(helpers->completed_count() == helpers->submitted_count());

    idle_client.close();
    file_client.close();
}

void stop_drains_active_write_then_closes() {
    // Session that enters writing state immediately on initial_action or read
    http::HttpSessionConfig hcfg;
    hcfg.keep_alive = true;
    hcfg.response_body = std::string(32 * 1024, 'D');
    hcfg.max_output_bytes = 64 * 1024;

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    // Small byte budget per turn so writing takes multiple turns
    cfg.budgets.max_bytes_per_session_turn = 2048;

    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        }
    ));
    CHECK(driver->start());
    const auto ep = driver->local_endpoint();

    LoopbackClient client(ep);
    client.write_all("GET /drain HTTP/1.1\r\nHost: localhost\r\n\r\n");

    // Wait until some bytes have been written by the loop
    const auto wait_deadline = test::Clock::now() + 2s;
    while (driver->stats().total_bytes_written == 0 && test::Clock::now() < wait_deadline) {
        std::this_thread::sleep_for(1ms);
    }

    // Request stop with generous drain timeout (1000ms) so in-flight write finishes
    driver->request_stop(1000ms);

    auto resp = client.read_http_response(http::Method::GET);
    CHECK(resp.status_code == 200 && resp.body.size() == 32 * 1024);

    driver->join();
    CHECK(driver->stats().active_sessions == 0);
}

void stale_completions_and_reused_generations_ignored() {
    // Verify that if a slow helper completion from transaction 1 arrives after
    // the session has moved on or closed, it is safely discarded.
    auto helpers = std::make_shared<AsyncTestWorkExecutor>(2, 64);

    // Custom work executor that injects an extra stale completion alongside the real one
    class StaleInjectingExecutor final : public WorkExecutor {
    public:
        explicit StaleInjectingExecutor(std::shared_ptr<AsyncTestWorkExecutor> inner)
            : inner_(std::move(inner)) {}

        core::Result<void> submit(WorkAction action, Completion completion) override {
            // Deliver a bogus stale completion first (wrong generation and wrong op seq)
            WorkEvent stale1;
            stale1.id = action.id;
            stale1.id.transaction.connection.generation += 99;
            stale1.status = WorkStatus::complete;
            completion(stale1);

            WorkEvent stale2;
            stale2.id = action.id;
            stale2.id.sequence += 99;
            stale2.status = WorkStatus::complete;
            completion(stale2);

            return inner_->submit(std::move(action), std::move(completion));
        }

    private:
        std::shared_ptr<AsyncTestWorkExecutor> inner_;
    };

    test::TemporaryDirectory dir;
    const auto path = dir.write("ok.txt", "stale-check-ok");
    auto injector = std::make_shared<StaleInjectingExecutor>(helpers);

    http::HttpSessionConfig hcfg;
    hcfg.application = http::make_file_reference_factory(path);
    hcfg.keep_alive = true;

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);
    auto driver = test::take(make_event_loop_driver(
        cfg,
        [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
            return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
        },
        injector
    ));
    CHECK(driver->start());

    LoopbackClient client(driver->local_endpoint());
    client.write_all(
        "GET /1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /2 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    );
    auto r1 = client.read_http_response();
    auto r2 = client.read_http_response();
    CHECK(r1.status_code == 200 && r1.body == "stale-check-ok");
    CHECK(r2.status_code == 200 && r2.body == "stale-check-ok");

    driver->request_stop(500ms);
    driver->join();
    CHECK(driver->stats().stale_completions_dropped >= 4);
}

void repeated_start_stop_and_idempotent_lifecycle() {
    http::HttpSessionConfig hcfg;
    hcfg.response_body = "lifecycle-cycle";

    EventLoopConfig cfg;
    cfg.endpoint = net::Endpoint::ipv4_loopback(0);

    for (int cycle = 0; cycle < 5; ++cycle) {
        auto driver = test::take(make_event_loop_driver(
            cfg,
            [hcfg](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
                return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(hcfg, id));
            }
        ));

        // Idempotent join before start
        driver->join();
        CHECK(driver->start());
        // Second start while running fails cleanly
        CHECK(!driver->start());

        LoopbackClient client(driver->local_endpoint());
        client.write_all("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        auto resp = client.read_http_response();
        CHECK(resp.status_code == 200 && resp.body == "lifecycle-cycle");
        client.close();

        // Repeated request_stop and join are idempotent
        driver->request_stop(200ms);
        driver->request_stop(50ms);
        driver->join();
        driver->join();
        CHECK(!driver->is_running());
        CHECK(driver->stats().active_sessions == 0);
    }
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    stop_during_idle_and_active_file_work();
    stop_drains_active_write_then_closes();
    stale_completions_and_reused_generations_ignored();
    repeated_start_stop_and_idempotent_lifecycle();
    return 0;
}
