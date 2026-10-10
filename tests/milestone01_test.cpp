#include "test_support.hpp"
using namespace std::chrono_literals;

#include <array>
#include <memory>

int main() {
    static_assert(!std::is_copy_constructible_v<net::Socket>);
    static_assert(std::is_nothrow_move_constructible_v<net::Socket>);
    auto runtime = test::take(net::initialize_network());
    auto another = test::take(net::initialize_network());
    another = std::move(runtime);
    CHECK(another.is_initialized());
    CHECK(!runtime.is_initialized());
    // All sockets below die before the runtime that owns initialization.
    net::Socket invalid;
    CHECK(!invalid.is_valid());
    CHECK(!invalid.bind(net::Endpoint::ipv4_loopback(0)));
    CHECK(invalid.accept().error().category == core::ErrorCategory::bad_descriptor);
    auto first = test::listener();
    auto occupied = test::take(first.local_endpoint());
    auto second = test::take(net::create_tcp_socket());
    auto collision = second.bind(occupied);
    CHECK(!collision);
    CHECK(collision.error().native_code != 0);
    CHECK(collision.error().category == core::ErrorCategory::address_in_use);
    // Move assignment must close the destination's old handle.
    second = std::move(first);
    CHECK(!first.is_valid());
    first.close();
    first.close();
    CHECK(second.local_endpoint().value() == occupied);
    auto& self = second;
    second = std::move(self); // self-move remains usable
    CHECK(second.is_valid());
    second.close();
    for (int i = 0; i < 64; ++i) {
        auto replacement = test::listener();
        second.close();
        CHECK(replacement.local_endpoint());
    }
    auto pair = test::pair(); // helper destroys the listener after accept
    CHECK(pair.client.remote_endpoint());
    CHECK(pair.server.remote_endpoint());
    std::array<std::byte, 1> byte{std::byte{0x87}};
    CHECK(pair.client.write_some(byte).bytes_transferred == 1);
    auto deadline = test::Clock::now() + 2s;
    for (;;) {
        std::array<std::byte, 1> received{};
        auto result = pair.server.read_some(received);
        if (result.is_ok()) { CHECK(received == byte); break; }
        CHECK(result.would_block());
        CHECK(test::Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    core::Result<std::unique_ptr<int>> value(std::make_unique<int>(42));
    auto moved = std::move(value);
    CHECK(*moved.value() == 42);
    core::Result<int> failed(core::make_error(core::ErrorCategory::io_error, "test", 7));
    CHECK(failed.has_error());
    CHECK(failed.error().native_code == 7);
    bool threw = false;
    try { (void)failed.value(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
