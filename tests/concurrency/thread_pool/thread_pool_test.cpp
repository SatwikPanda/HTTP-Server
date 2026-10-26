#include "concurrency/thread_pool/thread_pool_driver.hpp"

#include "fixtures/parallel_io.hpp"
#include "fixtures/io_controls.hpp"
#include "fixtures/session_probe.hpp"
#include "http/http_session.hpp"
#include "http/reference_application.hpp"
#include "net/runtime.hpp"
#include "test_support.hpp"
#ifdef THREAD_POOL_TEST_STATIC
#include "server/static_service.hpp"
#endif

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace concurrency;
using namespace concurrency::thread_pool;
using namespace test;

namespace {

// ---- loopback client helpers (nonblocking, deadline-bounded) -----------------

net::Socket connect_to(const net::Endpoint& endpoint) {
    auto socket = take(net::create_tcp_socket());
    CHECK(socket.connect(endpoint));
    CHECK(socket.set_nonblocking(true));
    return socket;
}

void send_all(net::Socket& socket, std::string_view text, Clock::duration timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto bytes = std::as_bytes(std::span(text.data() + offset, text.size() - offset));
        auto wrote = socket.write_some(bytes);
        if (wrote.is_ok()) { CHECK(wrote.bytes_transferred > 0); offset += wrote.bytes_transferred; continue; }
        CHECK(wrote.would_block());
        CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

// Reads until `want` bytes, EOF/reset, or the deadline. `closed` reports EOF/reset.
std::string receive(net::Socket& socket, std::size_t want, Clock::duration timeout, bool* closed = nullptr) {
    const auto deadline = Clock::now() + timeout;
    std::string out;
    std::byte buffer[16384];
    if (closed) *closed = false;
    while (out.size() < want && Clock::now() < deadline) {
        auto read = socket.read_some(buffer);
        if (read.is_ok()) { out.append(reinterpret_cast<const char*>(buffer), read.bytes_transferred); continue; }
        if (read.would_block()) { std::this_thread::sleep_for(1ms); continue; }
        if (closed) *closed = true; // EOF or reset both mean the server closed
        break;
    }
    return out;
}

bool closes_within(net::Socket& socket, Clock::duration timeout) {
    bool closed = false;
    (void)receive(socket, std::numeric_limits<std::size_t>::max(), timeout, &closed);
    return closed;
}

template <class Predicate> void eventually(Predicate predicate, Clock::duration timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) { CHECK(Clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}

// ---- driver setup -------------------------------------------------------------

SessionFactory echo_factory(std::chrono::milliseconds idle = 3000ms, std::size_t window = 64 * 1024) {
    return [=](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
        return std::unique_ptr<ConnectionSession>(std::make_unique<EchoSession>(id, window, window, idle));
    };
}

SessionFactory http_factory(http::HttpSessionConfig config) {
    return [config](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
        return std::unique_ptr<ConnectionSession>(std::make_unique<http::HttpSession>(config, id));
    };
}

const auto overload = std::make_shared<const std::vector<std::byte>>([] {
    const std::string text = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    auto bytes = std::as_bytes(std::span(text));
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}());

std::unique_ptr<ThreadPoolDriver> start_driver(ThreadPoolConfig config, SessionFactory sessions,
                                               std::shared_ptr<helpers::HelperPool> helper_pool = nullptr) {
    ThreadPoolDependencies deps;
    deps.listener = listener();
    deps.sessions = std::move(sessions);
    deps.helpers = std::move(helper_pool);
    deps.overload_response = overload;
    auto driver = take(ThreadPoolDriver::create(std::move(config), std::move(deps)));
    CHECK(driver->start());
    return driver;
}

std::string echo_roundtrip(const net::Endpoint& endpoint, const std::string& payload) {
    auto client = connect_to(endpoint);
    send_all(client, payload);
    CHECK(client.shutdown_write());
    bool closed = false;
    auto echoed = receive(client, payload.size() + 1, 5s, &closed);
    CHECK(closed);
    return echoed;
}

// ---- cases --------------------------------------------------------------------

void several_clients_progress_with_fixed_workers() {
    ThreadPoolConfig config;
    config.workers = 3;
    auto driver = start_driver(config, echo_factory());
    std::vector<std::thread> clients;
    std::atomic<int> ok{};
    for (int i = 0; i < 8; ++i) {
        clients.emplace_back([&, i] {
            const std::string payload(1000 + i * 977, char('A' + i));
            if (echo_roundtrip(driver->local_endpoint(), payload) == payload) ++ok;
        });
    }
    for (auto& client : clients) client.join();
    CHECK(ok.load() == 8);
    eventually([&] { return driver->stats().sessions_finished == 8; });
    const auto stats = driver->stats();
    CHECK(stats.accepted == 8 && stats.peak_active <= 3 && stats.active == 0);
    driver->request_stop(0ms);
    driver->join();
}

void busy_worker_does_not_block_others() {
    ThreadPoolConfig config;
    config.workers = 2;
    auto driver = start_driver(config, echo_factory(5000ms));
    auto idle = connect_to(driver->local_endpoint()); // holds one worker
    eventually([&] { return driver->stats().active == 1; });
    for (int i = 0; i < 3; ++i) {
        const std::string payload = "request-" + std::to_string(i);
        CHECK(echo_roundtrip(driver->local_endpoint(), payload) == payload);
    }
    // The idle client still progresses on its own worker.
    send_all(idle, "still here");
    CHECK(receive(idle, 10, 3s) == "still here");
    driver->request_stop(0ms);
    driver->join();
    CHECK(closes_within(idle, 2s));
}

void full_queue_rejects_promptly_and_recovers() {
    ThreadPoolConfig config;
    config.workers = 1;
    config.accept_queue = 1;
    config.queue_timeout = 10000ms;
    auto driver = start_driver(config, echo_factory(5000ms));
    auto busy = connect_to(driver->local_endpoint());
    eventually([&] { return driver->stats().active == 1; });
    auto queued = connect_to(driver->local_endpoint());
    eventually([&] { return driver->stats().queued == 1; });
    auto excess = connect_to(driver->local_endpoint());
    bool closed = false;
    const auto response = receive(excess, std::numeric_limits<std::size_t>::max(), 3s, &closed);
    CHECK(closed && response.rfind("HTTP/1.1 503", 0) == 0);
    auto stats = driver->stats();
    CHECK(stats.rejected_full == 1 && stats.queued == 1 && stats.active == 1 && stats.peak_queued == 1);
    // Pressure ends: the queued client is served, then new clients are accepted.
    CHECK(busy.shutdown_write());
    CHECK(closes_within(busy, 3s));
    send_all(queued, "queued");
    CHECK(queued.shutdown_write());
    CHECK(receive(queued, 7, 3s) == "queued");
    CHECK(echo_roundtrip(driver->local_endpoint(), "after") == "after");
    driver->request_stop(0ms);
    driver->join();
}

void queued_client_cannot_evade_timeouts() {
    ThreadPoolConfig config;
    config.workers = 1;
    config.accept_queue = 4;
    config.queue_timeout = 150ms;
    auto driver = start_driver(config, echo_factory(5000ms));
    auto busy = connect_to(driver->local_endpoint());
    eventually([&] { return driver->stats().active == 1; });
    auto waiting = connect_to(driver->local_endpoint());
    const auto started = Clock::now();
    bool closed = false;
    const auto response = receive(waiting, std::numeric_limits<std::size_t>::max(), 3s, &closed);
    CHECK(closed && response.rfind("HTTP/1.1 503", 0) == 0);
    CHECK(Clock::now() - started < 2s); // swept by the acceptor while the worker stayed busy
    CHECK(driver->stats().queue_timeouts == 1);
    CHECK(busy.shutdown_write());
    CHECK(closes_within(busy, 3s));
    CHECK(echo_roundtrip(driver->local_endpoint(), "recovered") == "recovered");
    driver->request_stop(0ms);
    driver->join();
}

void slow_reader_large_echo_preserves_offsets() {
    ThreadPoolConfig config;
    config.workers = 1;
    auto driver = start_driver(config, echo_factory(5000ms, 8 * 1024));
    std::string payload(3 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = char((i * 131 + i / 7) & 0xff);
    auto client = connect_to(driver->local_endpoint());
    std::string echoed;
    std::thread writer([&] { send_all(client, payload, 20s); });
    std::this_thread::sleep_for(200ms); // the server meets a full socket and must wait
    echoed = receive(client, payload.size(), 20s);
    writer.join();
    CHECK(echoed == payload);
    driver->request_stop(0ms);
    driver->join();
}

void keep_alive_idle_limit_frees_worker() {
    TemporaryDirectory dir;
    const auto path = dir.write("page.txt", "keep-alive body");
    http::HttpSessionConfig session;
    session.keep_alive = true;
    session.idle_timeout = 150ms;
    session.max_requests_per_connection = 2;
    session.application = http::make_file_reference_factory(path);
    auto helper_pool = take(helpers::HelperPool::start({}));
    ThreadPoolConfig config;
    config.workers = 1;
    auto driver = start_driver(config, http_factory(session), helper_pool);
    auto client = connect_to(driver->local_endpoint());
    send_all(client, "GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    const auto first = receive(client, 4096, 1s);
    CHECK(first.find("200 OK") != std::string::npos && first.find("keep-alive body") != std::string::npos);
    // Idle on a persistent connection: the session's idle limit releases the worker.
    CHECK(closes_within(client, 3s));
    auto next = connect_to(driver->local_endpoint());
    send_all(next, "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    bool closed = false;
    const auto second = receive(next, 1 << 20, 3s, &closed);
    CHECK(closed && second.find("keep-alive body") != std::string::npos);
    CHECK(helper_pool->stats().executed >= 4); // open/read/... ran on helpers
    driver->request_stop(0ms);
    driver->join();
    helper_pool->stop();
}

void request_limit_closes_persistent_connection() {
    TemporaryDirectory dir;
    const auto path = dir.write("page.txt", "x");
    http::HttpSessionConfig session;
    session.keep_alive = true;
    session.idle_timeout = 3000ms;
    session.max_requests_per_connection = 2;
    session.application = http::make_file_reference_factory(path);
    auto helper_pool = take(helpers::HelperPool::start({}));
    auto driver = start_driver({}, http_factory(session), helper_pool);
    auto client = connect_to(driver->local_endpoint());
    send_all(client, "GET / HTTP/1.1\r\nHost: a\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n");
    bool closed = false;
    const auto both = receive(client, 1 << 20, 3s, &closed);
    CHECK(closed); // closed by the request limit, well before the idle limit
    std::size_t count = 0;
    for (auto at = both.find("200 OK"); at != std::string::npos; at = both.find("200 OK", at + 1)) ++count;
    CHECK(count == 2);
    driver->request_stop(0ms);
    driver->join();
    helper_pool->stop();
}

#ifdef THREAD_POOL_TEST_STATIC
// Blocks only files whose name contains "slow"; production file code still runs.
class SlowFileAdapter final : public helpers::HelperAdapter {
public:
    WorkEvent execute(const WorkAction& work, const std::function<bool()>& stop) override {
        const auto* open = std::get_if<FileOpenWork>(&work.request);
        if (open && open->path.filename().string().find("slow") != std::string::npos) {
            std::unique_lock lock(mutex_);
            ++blocked_;
            cv_.notify_all();
            CHECK(cv_.wait_for(lock, 20s, [&] { return released_; }));
        }
        return inner_->execute(work, stop);
    }
    void wait_blocked() { std::unique_lock lock(mutex_); CHECK(cv_.wait_for(lock, 5s, [&] { return blocked_ > 0; })); }
    void release() { { std::lock_guard lock(mutex_); released_ = true; } cv_.notify_all(); }
private:
    std::shared_ptr<helpers::HelperAdapter> inner_ = helpers::make_reference_adapter();
    std::mutex mutex_;
    std::condition_variable cv_;
    int blocked_{};
    bool released_{};
};

void static_service_through_helpers_with_deadlines() {
    TemporaryDirectory dir;
    std::string big(200000, 'z');
    for (std::size_t i = 0; i < big.size(); i += 1000) big[i] = char('a' + (i / 1000) % 26);
    dir.write("big.txt", big);
    dir.write("slow.txt", "slow body");
    const auto root = dir.write("index.html", "<p>index</p>").parent_path();
    server::StaticConfig static_config;
    static_config.document_root = root;
    static_config.work_timeout = 200ms;
    http::HttpSessionConfig session;
    session.keep_alive = false;
    session.idle_timeout = 3000ms;
    session.application = take(server::make_static_factory(static_config));
    auto adapter = std::make_shared<SlowFileAdapter>();
    helpers::HelperConfig helper_config;
    helper_config.threads = 2;
    auto helper_pool = take(helpers::HelperPool::start(helper_config, adapter));
    ThreadPoolConfig config;
    config.workers = 2;
    auto driver = start_driver(config, http_factory(session), helper_pool);

    auto slow = connect_to(driver->local_endpoint());
    send_all(slow, "GET /slow.txt HTTP/1.1\r\nHost: a\r\n\r\n");
    adapter->wait_blocked();
    // The other worker and the other helper serve a large file meanwhile.
    auto fast = connect_to(driver->local_endpoint());
    send_all(fast, "GET /big.txt HTTP/1.1\r\nHost: a\r\n\r\n");
    bool closed = false;
    const auto body = receive(fast, 1 << 22, 5s, &closed);
    CHECK(closed && body.find("200 OK") != std::string::npos);
    CHECK(body.size() > big.size() && body.compare(body.size() - big.size(), big.size(), big) == 0);
    // The blocked open passes its deadline: the worker answers without the helper.
    const auto timed_out = receive(slow, 1 << 20, 5s, &closed);
    CHECK(closed && timed_out.rfind("HTTP/1.1 503", 0) == 0);
    CHECK(driver->stats().work_timeouts == 1);
    adapter->release(); // the late open result is stale and discarded with its file token
    eventually([&] { return helper_pool->stats().running == 0; });
    auto again = connect_to(driver->local_endpoint());
    send_all(again, "GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK(receive(again, 1 << 20, 3s, &closed).find("<p>index</p>") != std::string::npos);
    driver->request_stop(0ms);
    driver->join();
    helper_pool->stop();
}

void stop_during_helper_wait_does_not_wait_for_helper() {
    TemporaryDirectory dir;
    const auto root = dir.write("slow.txt", "slow").parent_path();
    server::StaticConfig static_config;
    static_config.document_root = root;
    static_config.work_timeout = 10000ms;
    http::HttpSessionConfig session;
    session.idle_timeout = 10000ms;
    session.application = take(server::make_static_factory(static_config));
    auto adapter = std::make_shared<SlowFileAdapter>();
    auto helper_pool = take(helpers::HelperPool::start({}, adapter));
    auto driver = start_driver({}, http_factory(session), helper_pool);
    auto client = connect_to(driver->local_endpoint());
    send_all(client, "GET /slow.txt HTTP/1.1\r\nHost: a\r\n\r\n");
    adapter->wait_blocked();
    const auto started = Clock::now();
    driver->request_stop(1000ms);
    driver->join();
    CHECK(Clock::now() - started < 2s); // no wait for the blocked helper
    CHECK(closes_within(client, 2s));
    adapter->release();
    helper_pool->stop();
}
#endif

void stop_closes_queued_and_active_sockets() {
    ThreadPoolConfig config;
    config.workers = 1;
    config.accept_queue = 4;
    auto driver = start_driver(config, echo_factory(10000ms));
    auto active = connect_to(driver->local_endpoint());
    eventually([&] { return driver->stats().active == 1; });
    auto queued = connect_to(driver->local_endpoint());
    eventually([&] { return driver->stats().queued == 1; });
    const auto started = Clock::now();
    driver->request_stop(0ms);
    driver->request_stop(0ms); // idempotent
    driver->join();
    CHECK(Clock::now() - started < 2s);
    CHECK(closes_within(active, 2s));
    CHECK(closes_within(queued, 2s));
    // The listener is closed: new connections are refused or reset.
    auto late = take(net::create_tcp_socket());
    CHECK(!late.connect(driver->local_endpoint()));
}

SessionFactory large_response_factory(SessionProbe& probe, const std::string& body) {
    http::HttpSessionConfig session;
    session.response_body = body;
    session.max_output_bytes = 16 * 1024 * 1024;
    session.retry_timeout = 10000ms;
    return [session, &probe](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
        return std::unique_ptr<ConnectionSession>(std::make_unique<ProbedSession>(
            std::make_unique<http::HttpSession>(session, id), probe));
    };
}

void stop_drains_pending_output_within_budget() {
    ThreadPoolConfig config;
    config.workers = 1;
    SessionProbe probe;
    const std::string payload(8 * 1024 * 1024, 'q');
    auto driver = start_driver(config, large_response_factory(probe, payload));
    auto client = connect_to(driver->local_endpoint());
    // A small request creates pending server output without first filling both
    // directions of an echo connection. Wait for observed backpressure.
    send_all(client, "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
    eventually([&] { return probe.blocked_writes.load() > 0; });
    CHECK(probe.bytes_written > 0 && probe.bytes_written < payload.size());
    driver->request_stop(5000ms);
    eventually([&] { return probe.stopped.load(); });
    bool closed = false;
    const auto wire = receive(client, payload.size() + 8192, 10s, &closed);
    driver->join();
    const auto response = inspect_fixed_wire_response(wire);
    CHECK(closed && response.valid && response.status_code == 200);
    CHECK(response.body == payload && response.total_wire_bytes == wire.size());
    CHECK(driver->stats().forced_closes == 0);
}

// A session that ignores stop requests: only the driver's drain deadline ends it.
class StubbornSession final : public ConnectionSession {
public:
    explicit StubbornSession(core::ConnectionId id) : id_(id) {}
    Action initial_action() override { return wait(); }
    Action on_event(const Event&) override { return wait(); }
    SessionState state() const noexcept override { return SessionState::waiting; }
    bool is_closed() const noexcept override { return false; }
    std::size_t input_limit() const noexcept override { return 1; }
    std::size_t output_limit() const noexcept override { return 1; }
    std::size_t peak_input_bytes() const noexcept override { return 0; }
    std::size_t peak_output_bytes() const noexcept override { return 0; }
    core::ConnectionId connection_id() const noexcept override { return id_; }
    std::chrono::milliseconds idle_timeout() const noexcept override { return std::chrono::milliseconds::zero(); }
    std::chrono::steady_clock::time_point last_progress_time() const noexcept override { return Clock::now(); }
    const core::Error& error() const noexcept override { return error_; }
private:
    Action wait() { return WaitAction{net::Interest::none, Clock::now() + 60s, {{id_, 1}, core::EndpointRole::downstream, ++seq_}}; }
    core::ConnectionId id_;
    std::uint64_t seq_{};
    core::Error error_;
};

void stop_deadline_bounds_draining_sessions() {
    ThreadPoolConfig config;
    config.workers = 2;
    SessionProbe probe;
    const std::string payload(8 * 1024 * 1024, 'r');
    auto driver = start_driver(config, large_response_factory(probe, payload));
    auto client = connect_to(driver->local_endpoint());
    send_all(client, "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
    eventually([&] { return probe.blocked_writes.load() > 0; });
    auto started = Clock::now();
    driver->request_stop(200ms);
    driver->join();
    const auto drained_for = Clock::now() - started;
    CHECK(drained_for >= 150ms && drained_for < 2s);
    CHECK(probe.stopped && probe.bytes_written < payload.size());

    SessionFactory stubborn = [](core::ConnectionId id) -> core::Result<std::unique_ptr<ConnectionSession>> {
        return std::unique_ptr<ConnectionSession>(std::make_unique<StubbornSession>(id));
    };
    auto forced = start_driver({}, stubborn);
    auto held = connect_to(forced->local_endpoint());
    eventually([&] { return forced->stats().active == 1; });
    started = Clock::now();
    forced->request_stop(200ms);
    forced->join();
    const auto elapsed = Clock::now() - started;
    CHECK(elapsed >= 150ms && elapsed < 2s);
    CHECK(forced->stats().forced_closes == 1);
    CHECK(closes_within(held, 2s));
}

void lifecycle_edges_are_safe() {
    // Stop before start, destroy without start.
    {
        ThreadPoolDependencies deps;
        deps.listener = listener();
        deps.sessions = echo_factory();
        auto driver = take(ThreadPoolDriver::create({}, std::move(deps)));
        driver->request_stop(0ms);
        CHECK(!driver->start());
        driver->join();
    }
    {
        ThreadPoolDependencies deps;
        deps.listener = listener();
        deps.sessions = echo_factory();
        auto driver = take(ThreadPoolDriver::create({}, std::move(deps)));
    }
    // Invalid input.
    ThreadPoolConfig bad;
    bad.workers = 0;
    CHECK(!validate_config(bad));
    {
        ThreadPoolDependencies deps;
        deps.sessions = echo_factory();
        CHECK(!ThreadPoolDriver::create({}, std::move(deps))); // no listener
    }
    // Start twice, repeated start/stop.
    for (int round = 0; round < 10; ++round) {
        auto driver = start_driver({}, echo_factory());
        CHECK(!driver->start());
        if (round % 2 == 0) CHECK(echo_roundtrip(driver->local_endpoint(), "r") == "r");
        driver->request_stop(0ms);
        driver->join();
        driver->join();
    }
    // Partial startup: a worker or the acceptor fails; started threads are joined.
    for (std::size_t failing : {std::size_t{2}, std::size_t{4}}) {
        ThreadPoolConfig config;
        config.workers = 4;
        config.before_thread_start = [failing](std::size_t index) -> core::Result<void> {
            if (index == failing) return core::make_error(core::ErrorCategory::system_error, "injected start failure");
            return {};
        };
        ThreadPoolDependencies deps;
        deps.listener = listener();
        deps.sessions = echo_factory();
        auto driver = take(ThreadPoolDriver::create(config, std::move(deps)));
        auto started = driver->start();
        CHECK(!started && started.error().operation == "injected start failure");
        driver->join();
    }
}

void failing_session_factory_closes_connection() {
    std::atomic<int> calls{};
    SessionFactory failing = [&](core::ConnectionId) -> core::Result<std::unique_ptr<ConnectionSession>> {
        ++calls;
        return core::make_error(core::ErrorCategory::would_block, "session admission");
    };
    auto driver = start_driver({}, failing);
    auto client = connect_to(driver->local_endpoint());
    CHECK(closes_within(client, 3s));
    eventually([&] { return driver->stats().session_failures == 1; });
    driver->request_stop(0ms);
    driver->join();
    CHECK(calls.load() == 1);
}

} // namespace

int main() {
    auto runtime = take(net::initialize_network());
    several_clients_progress_with_fixed_workers();
    busy_worker_does_not_block_others();
    full_queue_rejects_promptly_and_recovers();
    queued_client_cannot_evade_timeouts();
    slow_reader_large_echo_preserves_offsets();
    keep_alive_idle_limit_frees_worker();
    request_limit_closes_persistent_connection();
#ifdef THREAD_POOL_TEST_STATIC
    static_service_through_helpers_with_deadlines();
    stop_during_helper_wait_does_not_wait_for_helper();
#else
    std::cout << "UNSUPPORTED: static service module absent; static helper cases not run\n";
#endif
    stop_closes_queued_and_active_sockets();
    stop_drains_pending_output_within_budget();
    stop_deadline_bounds_draining_sessions();
    lifecycle_edges_are_safe();
    failing_session_factory_closes_connection();
    std::cout << "milestone 23 thread-pool tests passed\n";
    return 0;
}
