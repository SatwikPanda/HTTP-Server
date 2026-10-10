#include "test_support.hpp"
using namespace std::chrono_literals;

#include "net/poller.hpp"
#include "net/resolver.hpp"
#include "net/echo_server.hpp"

#include <array>
#include <atomic>
#include <future>
#include <vector>

namespace {
void readiness_and_generations() {
    auto poller = test::take(net::Poller::create());
    auto pair = test::pair();
    net::ConnectionId owner{9, 2};
    auto token = test::take(poller.watch(pair.server, net::Interest::read, owner));
    CHECK(!poller.watch(pair.server, net::Interest::read, owner));
    CHECK(test::take(poller.wait(test::Clock::now() + 25ms)).empty());
    std::array<std::byte, 3> data{std::byte{0}, std::byte{255}, std::byte{7}};
    CHECK(pair.client.write_some(data).bytes_transferred == data.size());
    CHECK(pair.client.shutdown_write());
    auto events = test::take(poller.wait(test::Clock::now() + 1s));
    CHECK(events.size() == 1);
    auto event = events.front();
    CHECK(event.readable || event.hangup);
    CHECK(event.owner == owner);
    CHECK(poller.is_current(event));
    CHECK(!test::take(poller.wait(test::Clock::now())).empty()); // level triggered
    CHECK(poller.unwatch(token));
    CHECK(!poller.is_current(event));
    CHECK(!poller.modify(token, net::Interest::write));
    CHECK(!poller.unwatch(token));
    auto replacement = test::take(poller.watch(pair.server, net::Interest::read, {9, 3}));
    CHECK(replacement != token);
    CHECK(!poller.is_current(event));
    std::array<std::byte, 3> received{};
    CHECK(pair.server.read_some(received).bytes_transferred == data.size());
    CHECK(received == data); // drain bytes even with hangup
    CHECK(poller.unwatch(replacement));
    auto other_poller = test::take(net::Poller::create());
    CHECK(!other_poller.modify(token, net::Interest::write));
    auto forged_owner = event;
    forged_owner.owner.generation++;
    CHECK(!poller.is_current(forged_owner));
    auto moved_token = test::take(poller.watch(pair.server, net::Interest::write, owner));
    auto moved = std::move(pair.server);
    events = test::take(poller.wait(test::Clock::now() + 1s));
    CHECK(!events.empty() && events.front().writable);
    CHECK(poller.is_current(events.front()));
    CHECK(poller.unwatch(moved_token));
    moved.close();
    auto fresh_pair = test::pair();
    auto closed_token = test::take(poller.watch(fresh_pair.server, net::Interest::write, owner));
    events = test::take(poller.wait(test::Clock::now() + 1s));
    fresh_pair.server.close(); // defensive check against a caller lifetime violation
    CHECK(!poller.is_current(events.front()));
    CHECK(test::take(poller.wait(test::Clock::now())).empty());
    CHECK(poller.unwatch(closed_token));
    for (std::uint64_t generation = 4; generation < 36; ++generation) {
        auto recycled = test::pair();
        auto registration = test::take(poller.watch(recycled.server, net::Interest::write, {9, generation}));
        auto current = test::take(poller.wait(test::Clock::now() + 1s));
        CHECK(current.size() == 1 && poller.is_current(current.front()));
        CHECK(!poller.is_current(event));
        CHECK(poller.modify(registration, net::Interest::read));
        CHECK(test::take(poller.wait(test::Clock::now() + 2ms)).empty());
        CHECK(poller.unwatch(registration));
        recycled.server.close();
        CHECK(!poller.is_current(current.front()));
    }
}

void wake_and_deadlines() {
    auto expired_create = net::Poller::create(test::Clock::now() - 1ms);
    CHECK(!expired_create && expired_create.error().category == core::ErrorCategory::timed_out);
    auto poller = test::take(net::Poller::create());
    auto start = test::Clock::now();
    CHECK(test::take(poller.wait(start - 1ms)).empty());
    CHECK(test::Clock::now() - start < 250ms);
    start = test::Clock::now();
    CHECK(test::take(poller.wait(start + 40ms)).empty());
    CHECK(test::Clock::now() - start >= 30ms);
    auto waiting = std::async(std::launch::async, [&] {
        return poller.wait(test::Clock::now() + 5s);
    });
    std::this_thread::sleep_for(30ms);
    CHECK(poller.wake());
    CHECK(waiting.wait_for(1s) == std::future_status::ready);
    CHECK(test::take(waiting.get()).empty());
    std::vector<std::jthread> wakers;
    for (int i = 0; i < 4; ++i) wakers.emplace_back([&] {
        for (int j = 0; j < 5000; ++j) CHECK(poller.wake());
    });
    wakers.clear();
    CHECK(test::take(poller.wait(test::Clock::now() + 1s)).empty());
    start = test::Clock::now();
    CHECK(test::take(poller.wait(start + 30ms)).empty());
    CHECK(test::Clock::now() - start >= 20ms); // wake channel fully drained
    for (int i = 0; i < 32; ++i) {
        auto waiting_again = std::async(std::launch::async, [&] {
            return poller.wait(test::Clock::now() + 2s);
        });
        CHECK(poller.wake()); // may race before or after the native wait starts
        CHECK(waiting_again.wait_for(1s) == std::future_status::ready);
        CHECK(test::take(waiting_again.get()).empty());
    }
}

void outbound(net::AddressFamily family) {
    auto created = net::create_tcp_socket(family);
    if (!created && family == net::AddressFamily::ipv6) {
        std::cout << "IPv6 unavailable: " << created.error().to_string() << '\n'; return;
    }
    auto listener = test::take(std::move(created));
    auto bound = listener.bind(family == net::AddressFamily::ipv6
        ? net::Endpoint::ipv6_loopback(0) : net::Endpoint::ipv4_loopback(0));
    if (!bound && family == net::AddressFamily::ipv6 &&
        bound.error().category == core::ErrorCategory::address_not_available) {
        std::cout << "IPv6 loopback unavailable\n"; return;
    }
    CHECK(bound);
    CHECK(listener.listen());
    CHECK(listener.set_nonblocking(true));
    auto endpoint = test::take(listener.local_endpoint());
    auto client = test::take(net::create_tcp_socket(family));
    auto state = test::take(net::begin_connect(client, endpoint));
    auto poller = test::take(net::Poller::create());
    auto token = test::take(poller.watch(client, net::Interest::write, {2, 1}));
    if (state == net::ConnectState::in_progress) {
        CHECK(!test::take(poller.wait(test::Clock::now() + 2s)).empty());
    }
    CHECK(net::finish_connect(client));
    CHECK(net::finish_connect(client)); // connected completion is idempotent
    CHECK(poller.unwatch(token));
    auto accepted = test::take(listener.accept());
    CHECK(accepted.set_nonblocking(true));
    auto candidates = test::take(net::resolve({endpoint.address(), endpoint.port()}));
    CHECK(!candidates.empty());
    CHECK(candidates.front() == endpoint);
    auto via_candidates = test::take(net::connect_candidates(candidates, test::Clock::now() + 2s));
    CHECK(via_candidates.remote_endpoint());
    auto expired = net::connect_candidates(candidates, test::Clock::now() - 1ms);
    CHECK(!expired && expired.error().category == core::ErrorCategory::timed_out);
}

void refused_and_fallback() {
    auto reservation = test::listener();
    auto refused_endpoint = test::take(reservation.local_endpoint());
    auto listener = test::listener();
    auto live_endpoint = test::take(listener.local_endpoint());
    CHECK(refused_endpoint != live_endpoint);
    // A bound, non-listening TCP socket is not a portable refusal fixture:
    // Darwin silently drops SYNs that match a socket in TCPS_CLOSED. Release a
    // temporary listener so the target really is a closed port on every OS.
    reservation.close();
    auto client = test::take(net::create_tcp_socket());
    auto state = net::begin_connect(client, refused_endpoint);
    if (state) {
        CHECK(state.value() == net::ConnectState::in_progress);
        auto poller = test::take(net::Poller::create());
        auto token = test::take(poller.watch(client, net::Interest::write, {3, 1}));
        CHECK(!test::take(poller.wait(test::Clock::now() + 3s)).empty());
        auto completed = net::finish_connect(client);
        CHECK(!completed && completed.error().category == core::ErrorCategory::connection_refused);
        CHECK(!net::finish_connect(client)); // a consumed SO_ERROR cannot become success
        CHECK(poller.unwatch(token));
    } else CHECK(state.error().category == core::ErrorCategory::connection_refused);
    auto all_refused = net::connect_candidates(
        std::array{refused_endpoint}, test::Clock::now() + 4s);
    CHECK(!all_refused);
    CHECK(all_refused.error().category == core::ErrorCategory::connection_refused);
    std::array candidates{refused_endpoint, live_endpoint};
    auto connected = test::take(net::connect_candidates(candidates, test::Clock::now() + 4s));
    CHECK(connected.remote_endpoint().value() == candidates[1]);
    CHECK(!net::connect_candidates({}, test::Clock::now() + 1s));
    auto unconnected = test::take(net::create_tcp_socket());
    CHECK(!net::finish_connect(unconnected));
    CHECK(!net::resolve({"", 80}));
    CHECK(!net::resolve({std::string("local\0host", 10), 80}));
    CHECK(!net::resolve({"256.256.256.256", 80}));
    CHECK(!test::take(net::resolve({"localhost", 80})).empty());
    CHECK(!net::create_tcp_socket(net::AddressFamily::unspecified));
    CHECK(!net::Endpoint::from_string("127.0.0.999", 80));
    CHECK(!net::Endpoint::from_string(std::string("127.0.0.1\0bad", 13), 80));
    auto invalid_address = net::begin_connect(unconnected, {net::AddressFamily::ipv4, "bad", 80});
    CHECK(!invalid_address && invalid_address.error().category == core::ErrorCategory::invalid_argument);
    CHECK((net::HostPort{"::1", 80}.to_string() == "[::1]:80"));
}

void idle_echo_uses_readiness() {
    auto pair = test::pair();
    net::EchoConfig config;
    config.retry_timeout = 80ms;
    config.wait_retry_delay = 0ms; // real sockets must ignore the retry delay
    auto start = test::Clock::now();
    auto stats = net::run_echo_session(pair.server, config);
    CHECK(!stats.completed_cleanly);
    CHECK(stats.error.category == core::ErrorCategory::timed_out);
    CHECK(stats.readiness_wait_count == 1);
    CHECK(stats.would_block_read_count == 1);
    CHECK(test::Clock::now() - start >= 70ms);
}

void stoppable_echo() {
    net::EchoConfig config;
    config.endpoint = net::Endpoint::ipv4_loopback(0);
    config.retry_timeout = 5s;
    net::EchoServer server(config);
    CHECK(server.start());
    auto running = std::async(std::launch::async, [&] { return server.run(); });
    std::this_thread::sleep_for(30ms);
    server.stop(); // wake a listener wait without closing a different thread's socket
    CHECK(running.wait_for(1s) == std::future_status::ready);
    CHECK(running.get());
    CHECK(!server.is_running());
    CHECK(server.start());
    auto client = test::take(net::connect_candidates(
        std::array{server.local_endpoint()}, test::Clock::now() + 1s));
    running = std::async(std::launch::async, [&] { return server.run(); });
    std::this_thread::sleep_for(30ms);
    server.stop(); // wake an idle client session too
    CHECK(running.wait_for(1s) == std::future_status::ready);
    CHECK(running.get());
    // A stop racing just before driver entry must not auto-restart the server.
    running = std::async(std::launch::async, [&] { return server.run(); });
    CHECK(running.wait_for(1s) == std::future_status::ready);
    CHECK(running.get());
    for (int i = 0; i < 16; ++i) {
        CHECK(server.start());
        running = std::async(std::launch::async, [&] { return server.run(); });
        server.stop();
        CHECK(running.wait_for(1s) == std::future_status::ready);
        CHECK(running.get());
    }
}
}
int main() {
    auto runtime = test::take(net::initialize_network());
    readiness_and_generations();
    wake_and_deadlines();
    outbound(net::AddressFamily::ipv4);
    outbound(net::AddressFamily::ipv6);
    refused_and_fallback();
    idle_echo_uses_readiness();
    stoppable_echo();
}
