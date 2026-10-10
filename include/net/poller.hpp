#pragma once

#include "core/id.hpp"
#include "core/result.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace net {
using Deadline = std::chrono::steady_clock::time_point;
using ConnectionId = core::ConnectionId;

struct RegistrationToken {
    std::uint64_t poller{};
    std::uint64_t slot{};
    std::uint64_t generation{};
    bool operator==(const RegistrationToken&) const = default;
};

enum class Interest : unsigned { none = 0, read = 1, write = 2, read_write = 3 };

struct ReadyEvent {
    RegistrationToken token;
    ConnectionId owner;
    bool readable{};
    bool writable{};
    bool hangup{};
    bool error{};
};

// One thread owns registration, wait, and socket lifetimes. Only wake() may be
// called concurrently. Unwatch before close; validate saved events before use.
// NetworkRuntime must outlive the poller and every socket it watches.
class Poller {
public:
    [[nodiscard]] static core::Result<Poller> create(
        Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5));
    Poller(Poller&&) noexcept;
    Poller& operator=(Poller&&) noexcept;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;
    ~Poller();

    [[nodiscard]] core::Result<RegistrationToken> watch(Socket&, Interest, ConnectionId);
    [[nodiscard]] core::Result<void> modify(RegistrationToken, Interest);
    [[nodiscard]] core::Result<void> unwatch(RegistrationToken);
    [[nodiscard]] core::Result<std::vector<ReadyEvent>> wait(Deadline);
    [[nodiscard]] core::Result<void> wake();
    [[nodiscard]] bool is_current(const ReadyEvent&) const noexcept;

private:
    class Impl;
    explicit Poller(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] inline auto watch(Poller& p, Socket& s, Interest i, ConnectionId id) { return p.watch(s, i, id); }
[[nodiscard]] inline auto modify(Poller& p, RegistrationToken t, Interest i) { return p.modify(t, i); }
[[nodiscard]] inline auto unwatch(Poller& p, RegistrationToken t) { return p.unwatch(t); }
[[nodiscard]] inline auto wait(Poller& p, Deadline d) { return p.wait(d); }
[[nodiscard]] inline auto wake(Poller& p) { return p.wake(); }
} // namespace net
