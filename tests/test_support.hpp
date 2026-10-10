#pragma once

#include "net/runtime.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>
#include <utility>

// Unlike assert(), CHECK always evaluates its expression in Release builds.
#define CHECK(expression) do { if (!(expression)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression "\n"; \
    std::exit(EXIT_FAILURE); } } while (false)

namespace test {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

template<class T> T take(core::Result<T> result) {
    if (!result) {
        std::cerr << result.error().to_string() << '\n';
        std::exit(EXIT_FAILURE);
    }
    return std::move(result.value());
}

inline net::Socket listener(net::AddressFamily family = net::AddressFamily::ipv4) {
    auto socket = take(net::create_tcp_socket(family));
    CHECK(socket.bind(family == net::AddressFamily::ipv6
        ? net::Endpoint::ipv6_loopback(0) : net::Endpoint::ipv4_loopback(0)));
    CHECK(socket.listen());
    CHECK(socket.set_nonblocking(true));
    return socket;
}

struct Pair { net::Socket client; net::Socket server; };
inline Pair pair() {
    auto listening = listener();
    auto client = take(net::create_tcp_socket());
    CHECK(client.connect(take(listening.local_endpoint())));
    auto deadline = Clock::now() + 2s;
    for (;;) {
        auto accepted = listening.accept();
        if (accepted) {
            auto server = std::move(accepted.value());
            CHECK(client.set_nonblocking(true));
            CHECK(server.set_nonblocking(true));
            return {std::move(client), std::move(server)};
        }
        CHECK(accepted.error().category == core::ErrorCategory::would_block);
        CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

inline int pending(int milestone) {
    std::cout << "NOT IMPLEMENTED: milestone " << milestone
              << "; acceptance tests are pending, not passed.\n";
    return 77;
}
} // namespace test
