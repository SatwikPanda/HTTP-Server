#pragma once

// Declaration-only acceptance contract. No fake session implementation exists
// here. An adapter must supply definitions backed by the production session.
#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

namespace tests {
class SessionHarness {
public:
    SessionHarness(std::size_t input_limit, std::size_t output_limit,
                   std::chrono::milliseconds idle_timeout);
    void force_write_chunk(std::size_t);
    void block_next_writes(std::size_t);
    void receive(std::span<const std::byte>);
    void peer_eof();
    void run_until_idle();
    void advance(std::chrono::milliseconds);
    void stop(std::chrono::milliseconds);
    const std::vector<std::byte>& output() const;
    bool closed() const;
    std::size_t peak_input_bytes() const;
    std::size_t peak_output_bytes() const;
    std::size_t open_resources() const;
};
bool echo_session_real_socket_roundtrip(std::span<const std::byte>);
bool driver_accepts_two_sequential_clients();
} // namespace tests
