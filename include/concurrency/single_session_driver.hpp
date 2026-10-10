#pragma once

#include "concurrency/session.hpp"
#include "core/clock.hpp"
#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/poller.hpp"
#include "net/socket.hpp"
#include "net/stream.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace concurrency {

struct DriverStats {
    std::size_t total_bytes_read{0};
    std::size_t total_bytes_written{0};
    std::size_t short_write_count{0};
    std::size_t would_block_read_count{0};
    std::size_t would_block_write_count{0};
    std::size_t readiness_wait_count{0};
    core::Error error{};
    bool completed_cleanly{false};
};

class SingleSessionDriver {
public:
    explicit SingleSessionDriver(const core::SteadyClock& clock = core::real_steady_clock());
    ~SingleSessionDriver() = default;

    SingleSessionDriver(const SingleSessionDriver&) = delete;
    SingleSessionDriver& operator=(const SingleSessionDriver&) = delete;
    SingleSessionDriver(SingleSessionDriver&&) noexcept = default;
    SingleSessionDriver& operator=(SingleSessionDriver&&) noexcept = default;

    // Drive an accepted client socket with a ConnectionSession using Poller
    [[nodiscard]] DriverStats drive(
        net::Socket& socket,
        ConnectionSession& session,
        net::Poller& poller,
        net::RegistrationToken token,
        const std::function<bool()>& stop_requested = [] { return false; }
    );

    // Drive an abstract StreamChannel with a ConnectionSession
    [[nodiscard]] DriverStats drive_channel(
        net::StreamChannel& channel,
        ConnectionSession& session,
        std::chrono::milliseconds wait_retry_delay = std::chrono::milliseconds(1),
        const std::function<bool()>& stop_requested = [] { return false; }
    );

private:
    const core::SteadyClock* clock_{nullptr};
};

} // namespace concurrency
