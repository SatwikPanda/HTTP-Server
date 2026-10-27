#include "application/server.hpp"

#include "application/runtime.hpp"
#include "fixtures/io_controls.hpp"
#include "routes.hpp"
#include "test_support.hpp"
#include "net/poller.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

std::string exchange(const net::Endpoint& endpoint, std::string_view request) {
    auto client = test::take(net::create_tcp_socket(endpoint.family()));
    CHECK(client.connect(endpoint)); CHECK(client.set_nonblocking(true));
    auto poller = test::take(net::Poller::create());
    auto token = test::take(poller.watch(client, net::Interest::write, {99, 1}));
    const auto deadline = test::Clock::now() + 5s;
    const auto wait = [&](net::Interest interest) {
        CHECK(poller.modify(token, interest));
        while (test::Clock::now() < deadline) {
            const auto events = test::take(poller.wait(deadline));
            for (const auto& event : events) if (event.token == token && poller.is_current(event)) return;
        }
        CHECK(false);
    };
    const auto bytes = std::as_bytes(std::span(request.data(), request.size()));
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        CHECK(test::Clock::now() < deadline);
        const auto result = client.write_some(bytes.subspan(sent));
        if (result.is_ok()) { CHECK(result.bytes_transferred > 0); sent += result.bytes_transferred; }
        else { CHECK(result.would_block()); wait(net::Interest::write); }
    }
    std::string output;
    std::array<std::byte, 1024> buffer{};
    for (;;) {
        CHECK(test::Clock::now() < deadline);
        const auto result = client.read_some(buffer);
        if (result.is_eof()) break;
        if (result.is_ok()) {
            CHECK(result.bytes_transferred > 0 && output.size() + result.bytes_transferred <= 65536);
            output.append(reinterpret_cast<const char*>(buffer.data()), result.bytes_transferred);
        } else { CHECK(result.would_block()); wait(net::Interest::read); }
    }
    CHECK(poller.unwatch(token));
    return output;
}

void routes_over_tcp(core::DriverMode driver) {
    std::atomic<unsigned> calls{};
    application::ServerOptions options; options.driver = driver; options.workers = 2;
    application::Server server(options);
    CHECK(app::register_routes(server)); // Compile and execute the user's app routes.
    CHECK(server.get("/count", [&](const http::Request&) {
        ++calls; return http::Response::text("counted");
    }));
    CHECK(server.get("/failure", [](const http::Request&) -> http::Response { throw 42; }));
    const auto endpoint = test::take(server.listen(0));
    CHECK(endpoint.port() != 0 && endpoint.address() == "127.0.0.1");
    CHECK(!server.listen(0));
    CHECK(!server.get("/late", [](const http::Request&) { return http::Response::text("late"); }));
    const auto wire = exchange(endpoint,
        "GET /?ignored=1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    std::string_view rest = wire;
    const auto get = test::inspect_fixed_wire_response(rest); rest.remove_prefix(get.total_wire_bytes);
    const auto head = test::inspect_fixed_wire_response(rest, true); rest.remove_prefix(head.total_wire_bytes);
    const auto missing = test::inspect_fixed_wire_response(rest); rest.remove_prefix(missing.total_wire_bytes);
    CHECK(get.valid && get.status_code == 200 && get.body == "Hello, World!");
    CHECK(head.valid && head.status_code == 200 && head.body.empty() && head.has_header_line("Content-Length: 13"));
    CHECK(missing.valid && missing.status_code == 404 && rest.empty());
    const auto failed = test::inspect_fixed_wire_response(exchange(endpoint,
        "GET /failure HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    CHECK(failed.valid && failed.status_code == 500);

    std::vector<std::thread> clients;
    for (unsigned i = 0; i < 4; ++i) clients.emplace_back([&] {
        for (unsigned j = 0; j < 3; ++j) {
            const auto reply = test::inspect_fixed_wire_response(exchange(endpoint,
                "GET /count HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
            CHECK(reply.valid && reply.status_code == 200 && reply.body == "counted");
        }
    });
    for (auto& client : clients) client.join();
    CHECK(calls == 12);

    application::Server other(options);
    CHECK(!other.listen(endpoint.port())); // Failure must leave it retryable.
    CHECK(app::register_routes(other));
    CHECK(other.listen(0));
    other.request_stop(); other.wait();
    server.request_stop(); server.request_stop(); server.wait(); server.wait();
}

void validation_and_ownership() {
    application::Server invalid;
    CHECK(!invalid.listen(65536)); CHECK(!invalid.listen(0, "not-an-ip"));
    invalid.request_stop(); invalid.wait();
    application::ServerOptions options; options.workers = 0;
    application::Server bad_options(options); CHECK(!bad_options.listen(0));
    options.workers = 2; options.max_sessions = 1;
    application::Server too_many_workers(options); CHECK(!too_many_workers.listen(0));
    options.max_sessions = 2; options.accept_queue = 0;
    application::Server no_queue(options); CHECK(!no_queue.listen(0));
    core::RuntimeConfig config; config.application = core::ApplicationMode::proxy;
    http::Router router;
    CHECK(!application::Runtime::start(config, router.factory()));

    std::weak_ptr<int> lifetime;
    {
        application::Server owned;
        auto state = std::make_shared<int>(7); lifetime = state;
        CHECK(owned.get("/", [state](const http::Request&) { return http::Response::text(std::to_string(*state)); }));
        CHECK(owned.listen(0));
        state.reset(); CHECK(!lifetime.expired());
        // The destructor stops and joins even without an explicit stop/wait.
    }
    CHECK(lifetime.expired());
}
} // namespace

int main() {
    validation_and_ownership();
    for (auto driver : {core::DriverMode::single, core::DriverMode::thread_pool, core::DriverMode::event_loop}) {
        routes_over_tcp(driver);
    }
}
