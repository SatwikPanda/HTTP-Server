#pragma once

#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/socket.hpp"
#include "net/stream.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>

namespace net {

struct EchoConfig {
    Endpoint endpoint{Endpoint::ipv4_loopback(8080)};
    std::size_t buffer_capacity{4096};
    std::chrono::milliseconds wait_retry_delay{1};
    std::chrono::milliseconds retry_timeout{5000};
    std::size_t max_retries{5000};
};

struct EchoSessionStats {
    std::size_t total_bytes_read{0};
    std::size_t total_bytes_written{0};
    std::size_t short_write_count{0};
    std::size_t would_block_read_count{0};
    std::size_t would_block_write_count{0};
    bool completed_cleanly{false};
};

// Process an echo session on an abstract StreamChannel
EchoSessionStats run_echo_session(
    StreamChannel& channel,
    const EchoConfig& config
);

// Process an echo session on a concrete Socket
EchoSessionStats run_echo_session(
    Socket& socket,
    const EchoConfig& config
);

class EchoServer {
public:
    explicit EchoServer(EchoConfig config);
    ~EchoServer();

    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;

    [[nodiscard]] core::Result<void> start();
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    // Run synchronous accept loop until stop() is called or error occurs
    [[nodiscard]] core::Result<void> run();

    // Accept and handle a single client session
    [[nodiscard]] core::Result<void> accept_and_handle_one();

    [[nodiscard]] const Endpoint& local_endpoint() const noexcept {
        return actual_endpoint_;
    }

private:
    EchoConfig config_;
    Socket listener_;
    std::atomic<bool> running_{false};
    Endpoint actual_endpoint_;
};

} // namespace net
