#include "net/poller.hpp"
#include "detail/native.hpp"
#include "detail/socket_access.hpp"

#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace net {
namespace {
std::atomic<std::uint64_t> next_poller_id{1};
core::Error invalid(std::string operation) {
    return core::make_error(core::ErrorCategory::invalid_argument, std::move(operation), 0,
                            "Invalid, duplicate, or stale registration");
}
bool valid_interest(Interest interest) {
    return static_cast<unsigned>(interest) <= static_cast<unsigned>(Interest::read_write);
}
short native_interest(Interest interest) {
    short events = 0;
    if ((static_cast<unsigned>(interest) & 1) != 0) events |= POLLRDNORM;
    if ((static_cast<unsigned>(interest) & 2) != 0) events |= POLLWRNORM;
    return events;
}
}

class Poller::Impl {
public:
    struct Registration {
        detail::NativeSocket handle;
        std::weak_ptr<detail::SocketLifetime> lifetime;
        Interest interest;
        ConnectionId owner;
    };
    struct Slot {
        std::uint64_t generation{};
        std::optional<Registration> registration;
    };
    std::uint64_t id{next_poller_id.fetch_add(1, std::memory_order_relaxed)};
    std::vector<Slot> slots;
    Socket wake_reader, wake_writer;
    std::mutex wake_mutex;
    bool wake_pending{};

    Slot* find(RegistrationToken token) noexcept {
        if (token.poller != id || token.slot >= slots.size()) return nullptr;
        auto& slot = slots[static_cast<std::size_t>(token.slot)];
        return slot.generation == token.generation && slot.registration ? &slot : nullptr;
    }
};

Poller::Poller(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Poller::Poller(Poller&&) noexcept = default;
Poller& Poller::operator=(Poller&&) noexcept = default;
Poller::~Poller() = default;

core::Result<Poller> Poller::create(Deadline deadline) {
    if (std::chrono::steady_clock::now() >= deadline) {
        return core::make_error(core::ErrorCategory::timed_out, "create wake channel");
    }
    auto impl = std::make_unique<Impl>();
    auto created_listener = create_tcp_socket();
    if (!created_listener) return created_listener.error();
    auto listener = std::move(created_listener.value());
    auto bound = listener.bind(Endpoint::ipv4_loopback(0));
    if (!bound) return bound.error();
    auto listening = listener.listen(1);
    if (!listening) return listening.error();
    auto nonblocking = listener.set_nonblocking(true);
    if (!nonblocking) return nonblocking.error();
    auto endpoint = listener.local_endpoint();
    if (!endpoint) return endpoint.error();
    auto created_writer = create_tcp_socket();
    if (!created_writer) return created_writer.error();
    impl->wake_writer = std::move(created_writer.value());
    auto begun = impl->wake_writer.begin_connect(endpoint.value());
    if (!begun) return begun.error();
    bool connected = begun.value() == ConnectState::connected;
    for (;;) {
        detail::PollFd fds[2]{
            {detail::native_socket(detail::SocketAccess::handle(listener)), POLLRDNORM, 0},
            {detail::native_socket(detail::SocketAccess::handle(impl->wake_writer)), POLLWRNORM, 0}};
        // Once connect completes, wait only on accept readiness. Continuing to
        // poll an already writable socket would spin until the listener is ready.
        const int result = detail::poll_sockets(fds, connected ? 1 : 2, detail::timeout_ms(deadline));
        if (result < 0) {
            const int code = detail::native_error();
            if (detail::interrupted(code) && std::chrono::steady_clock::now() < deadline) continue;
            return detail::poll_error(code);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return core::make_error(core::ErrorCategory::timed_out, "create wake channel");
        }
        if (!connected && fds[1].revents != 0) {
            auto finished = impl->wake_writer.finish_connect();
            if (!finished) {
                if (finished.error().category != core::ErrorCategory::would_block) return finished.error();
            } else connected = true;
        }
        if (fds[0].revents == 0) continue;
        auto accepted = listener.accept();
        if (!accepted) {
            if (accepted.error().category == core::ErrorCategory::would_block) continue;
            return accepted.error();
        }
        impl->wake_reader = std::move(accepted.value());
        break;
    }
    if (!connected) {
        // Accept may arrive before the write readiness notification.
        auto finished = impl->wake_writer.finish_connect();
        if (!finished) return finished.error();
    }
    nonblocking = impl->wake_reader.set_nonblocking(true);
    if (!nonblocking) return nonblocking.error();
    return Poller(std::move(impl));
}

core::Result<RegistrationToken> Poller::watch(Socket& socket, Interest interest, ConnectionId owner) {
    if (!impl_ || !socket.is_valid() || !valid_interest(interest)) return invalid("watch");
    const auto handle = detail::native_socket(detail::SocketAccess::handle(socket));
    std::size_t available = impl_->slots.size();
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        auto& slot = impl_->slots[i];
        if (slot.registration && slot.registration->lifetime.expired()) slot.registration.reset();
        if (slot.registration && slot.registration->handle == handle) return invalid("watch");
        if (!slot.registration && slot.generation != std::numeric_limits<std::uint64_t>::max()) available = i;
    }
    if (available == impl_->slots.size()) impl_->slots.emplace_back();
    auto& slot = impl_->slots[available];
    ++slot.generation;
    slot.registration = Impl::Registration{handle, detail::SocketAccess::lifetime(socket), interest, owner};
    return RegistrationToken{impl_->id, available, slot.generation};
}

core::Result<void> Poller::modify(RegistrationToken token, Interest interest) {
    auto* slot = impl_ ? impl_->find(token) : nullptr;
    if (!slot || !valid_interest(interest) || slot->registration->lifetime.expired()) return invalid("modify");
    slot->registration->interest = interest;
    return {};
}

core::Result<void> Poller::unwatch(RegistrationToken token) {
    auto* slot = impl_ ? impl_->find(token) : nullptr;
    if (!slot) return invalid("unwatch");
    slot->registration.reset();
    return {};
}

bool Poller::is_current(const ReadyEvent& event) const noexcept {
    auto* slot = impl_ ? impl_->find(event.token) : nullptr;
    return slot && !slot->registration->lifetime.expired() && slot->registration->owner == event.owner;
}

core::Result<std::vector<ReadyEvent>> Poller::wait(Deadline deadline) {
    if (!impl_) return invalid("wait");
    std::vector<detail::PollFd> fds;
    std::vector<RegistrationToken> tokens;
    fds.push_back({detail::native_socket(detail::SocketAccess::handle(impl_->wake_reader)), POLLRDNORM, 0});
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        const auto& slot = impl_->slots[i];
        if (!slot.registration || slot.registration->lifetime.expired() || slot.registration->interest == Interest::none) continue;
        fds.push_back({slot.registration->handle, native_interest(slot.registration->interest), 0});
        tokens.push_back({impl_->id, i, slot.generation});
    }
    if (fds.size() > std::numeric_limits<unsigned int>::max()) return invalid("wait: too many sockets");
    for (;;) {
        const int result = detail::poll_sockets(fds.data(), fds.size(), detail::timeout_ms(deadline));
        if (result < 0) {
            const int code = detail::native_error();
            if (detail::interrupted(code)) {
                if (std::chrono::steady_clock::now() >= deadline) return std::vector<ReadyEvent>{};
                continue;
            }
            return detail::poll_error(code);
        }
        if (result == 0 && std::chrono::steady_clock::now() < deadline) continue;
        break;
    }
    if (fds[0].revents != 0) {
        std::lock_guard lock(impl_->wake_mutex);
        std::byte buffer[128];
        for (;;) {
            const auto read = impl_->wake_reader.read_some(buffer);
            if (read.would_block()) break;
            if (read.is_error()) return read.error;
            if (read.is_eof()) return core::make_error(core::ErrorCategory::io_error, "wake channel EOF");
        }
        impl_->wake_pending = false;
    }
    std::vector<ReadyEvent> events;
    for (std::size_t i = 1; i < fds.size(); ++i) {
        const auto flags = fds[i].revents;
        auto* slot = impl_->find(tokens[i - 1]);
        if (!flags || !slot || slot->registration->lifetime.expired()) continue;
        events.push_back({tokens[i - 1], slot->registration->owner,
                          (flags & POLLRDNORM) != 0, (flags & POLLWRNORM) != 0,
                          (flags & POLLHUP) != 0, (flags & (POLLERR | POLLNVAL)) != 0});
    }
    return events;
}

core::Result<void> Poller::wake() {
    if (!impl_) return invalid("wake");
    std::lock_guard lock(impl_->wake_mutex);
    if (impl_->wake_pending) return {};
    const std::byte byte{1};
    auto written = impl_->wake_writer.write_some(std::span(&byte, 1));
    if (written.is_error()) return written.error;
    if (written.is_ok() && written.bytes_transferred != 1) {
        return core::make_error(core::ErrorCategory::io_error, "wake", 0, "Wake made no progress");
    }
    // would_block means queued wake bytes already make the reader ready.
    impl_->wake_pending = true;
    return {};
}
} // namespace net
