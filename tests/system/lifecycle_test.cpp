#include "application/runtime.hpp"
#include "fixtures/io_controls.hpp"
#include "fixtures/parallel_io.hpp"
#include "test_support.hpp"
#include <functional>

namespace {
using namespace std::chrono_literals;

void await_state(const std::function<bool()>& predicate) {
    const auto deadline = test::Clock::now() + 3s;
    while (!predicate() && test::Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(predicate());
}

std::string receive_closed(net::Socket& socket) {
    CHECK(socket.set_nonblocking(true));
    auto poller = test::take(net::Poller::create());
    auto token = test::take(poller.watch(socket, net::Interest::read, {10, 1}));
    const auto deadline = test::Clock::now() + 3s;
    std::string output;
    std::array<std::byte, 1024> bytes;
    bool ended = false;
    while (test::Clock::now() < deadline) {
        auto read = socket.read_some(bytes);
        if (read.is_eof()) { ended = true; break; }
        if (read.is_ok()) {
            CHECK(read.bytes_transferred && output.size() < 8192);
            output.append(reinterpret_cast<const char*>(bytes.data()), read.bytes_transferred);
        } else if (read.would_block()) (void)test::take(poller.wait(deadline));
        else { ended = true; break; } // Immediate over-capacity reset is permitted.
    }
    CHECK(poller.unwatch(token));
    CHECK(ended);
    return output;
}

void pressure_and_recovery(core::DriverMode driver) {
    core::RuntimeConfig cfg;
    cfg.port = 0; cfg.driver = driver; cfg.workers = 1; cfg.sessions = 1;
    cfg.accept_queue = 1; cfg.helpers = 1; cfg.job_queue = 1; cfg.completion_queue = 1;
    cfg.idle_timeout = 5s; cfg.drain_timeout = 50ms;
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::cout << "pressure driver=" << static_cast<int>(driver) << " cycle=" << cycle << std::endl;
        auto app = test::take(application::Runtime::start(cfg));
        auto client = test::take(net::create_tcp_socket());
        CHECK(client.connect(app->local_endpoint()));
        await_state([&] { return app->statistics_json().find("\"active\":1") != std::string::npos; });
        auto queued = test::take(net::create_tcp_socket());
        if (driver == core::DriverMode::thread_pool) {
            CHECK(queued.connect(app->local_endpoint()));
            await_state([&] { return app->statistics_json().find("\"queued_accepts\":1") != std::string::npos; });
        }
        auto rejected = test::take(net::create_tcp_socket());
        CHECK(rejected.connect(app->local_endpoint()));
        auto wire = receive_closed(rejected);
        if (!wire.empty()) {
            auto response = test::inspect_fixed_wire_response(wire);
            CHECK(response.valid && response.status_code == 503 && response.total_wire_bytes == wire.size());
        }
        rejected.close(); queued.close(); client.close();
        await_state([&] {
            auto stats = app->statistics_json();
            return stats.find("\"active\":0") != std::string::npos &&
                (driver != core::DriverMode::thread_pool || stats.find("\"queued_accepts\":0") != std::string::npos);
        });
        auto recovered = test::take(net::create_tcp_socket());
        CHECK(recovered.connect(app->local_endpoint()));
        const std::string request = "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
        auto wrote = recovered.write_some(std::as_bytes(std::span(request)));
        CHECK(wrote.is_ok() && wrote.bytes_transferred == request.size());
        auto response = test::inspect_fixed_wire_response(receive_closed(recovered));
        CHECK(response.valid && response.status_code == 200 && response.body == "Hello, World!\r\n");
        app->request_stop(); app->request_stop(); app->join();
        CHECK(app->statistics_json().find("\"active\":0") != std::string::npos);
    }
}

void startup_rollback(core::DriverMode driver) {
    auto occupied = test::listener();
    core::RuntimeConfig cfg;
    cfg.driver = driver; cfg.port = test::take(occupied.local_endpoint()).port();
    CHECK(!application::Runtime::start(cfg));
    cfg.port = 0; cfg.application = core::ApplicationMode::static_files;
    test::TemporaryDirectory root;
    cfg.document_root = root.path() / "missing";
    CHECK(!application::Runtime::start(cfg));
    cfg.document_root = root.path();
    auto app = test::take(application::Runtime::start(cfg));
    app->request_stop(); app->join();
}
} // namespace

int main() {
    auto network = test::take(net::initialize_network());
    for (auto driver : {core::DriverMode::thread_pool, core::DriverMode::event_loop}) {
        pressure_and_recovery(driver);
        startup_rollback(driver);
    }
}
