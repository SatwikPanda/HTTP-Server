#pragma once
#include "concurrency/session.hpp"
#include <functional>

namespace concurrency {
using SessionFactory = std::function<core::Result<std::unique_ptr<ConnectionSession>>(core::ConnectionId)>;
class DriverLifecycle {
public:
    virtual ~DriverLifecycle() = default;
    [[nodiscard]] virtual core::Result<void> start() = 0;
    // Normal-thread API, idempotent; wakes the owner, which delivers StopEvent.
    virtual void request_stop(std::chrono::milliseconds drain_timeout) noexcept = 0;
    virtual void join() noexcept = 0;
};
} // namespace concurrency
