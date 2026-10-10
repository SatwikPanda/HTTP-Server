#include "net/echo_server.hpp"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace net {
namespace {
using WaitForProgress = std::function<bool(Interest, Deadline)>;
using StopRequested = std::function<bool()>;

EchoSessionStats echo(StreamChannel& channel, const EchoConfig& config,
                      const WaitForProgress& wait, const StopRequested& stopped) {
    EchoSessionStats stats{};
    std::vector<std::byte> buffer(std::max<std::size_t>(config.buffer_capacity, 1));
    std::size_t size = 0, offset = 0, retries = 0;
    auto last_progress = std::chrono::steady_clock::now();
    auto wait_for = [&](Interest interest) {
        ++retries;
        const auto deadline = last_progress + config.retry_timeout;
        if (retries > config.max_retries || std::chrono::steady_clock::now() >= deadline) {
            stats.error = core::make_error(core::ErrorCategory::timed_out, "echo progress");
            return false;
        }
        if (!wait(interest, deadline)) return false;
        return !stopped();
    };
    while (channel.is_valid() && !stopped()) {
        if (offset < size) {
            auto pending = std::span<const std::byte>(buffer.data() + offset, size - offset);
            const auto written = channel.write_some(pending);
            if (written.is_ok()) {
                const auto count = written.bytes_transferred;
                if (count == 0 || count > pending.size()) {
                    stats.error = core::make_error(core::ErrorCategory::io_error, "echo write", 0, "Invalid progress count");
                    break;
                }
                if (count < pending.size()) ++stats.short_write_count;
                offset += count;
                stats.total_bytes_written += count;
                last_progress = std::chrono::steady_clock::now();
                retries = 0;
                if (offset == size) { offset = 0; size = 0; }
                continue;
            }
            if (written.would_block()) {
                ++stats.would_block_write_count;
                if (!wait_for(Interest::write)) break;
                continue;
            }
            stats.error = written.error;
            break;
        }
        const auto read = channel.read_some(buffer);
        if (read.is_ok()) {
            if (read.bytes_transferred == 0 || read.bytes_transferred > buffer.size()) {
                stats.error = core::make_error(core::ErrorCategory::io_error, "echo read", 0, "Invalid progress count");
                break;
            }
            size = read.bytes_transferred;
            stats.total_bytes_read += size;
            last_progress = std::chrono::steady_clock::now();
            retries = 0;
        } else if (read.is_eof()) {
            stats.completed_cleanly = true;
            break;
        } else if (read.would_block()) {
            ++stats.would_block_read_count;
            if (!wait_for(Interest::read)) break;
        } else {
            stats.error = read.error;
            break;
        }
    }
    return stats;
}

EchoSessionStats ready_echo(Socket& socket, Poller& poller, RegistrationToken token,
                            const EchoConfig& config, const StopRequested& stopped) {
    SocketStreamChannel channel(socket);
    std::size_t waits = 0;
    core::Error wait_error;
    auto stats = echo(channel, config, [&](Interest interest, Deadline deadline) {
        // Writing is watched only when the bounded buffer has unsent bytes.
        auto modified = poller.modify(token, interest);
        if (!modified) { wait_error = modified.error(); return false; }
        while (!stopped()) {
            ++waits;
            auto events = poller.wait(deadline);
            if (!events) { wait_error = events.error(); return false; }
            if (std::chrono::steady_clock::now() >= deadline) {
                wait_error = core::make_error(core::ErrorCategory::timed_out, "echo readiness");
                return false;
            }
            for (const auto& event : events.value()) {
                if (event.token == token && poller.is_current(event)) {
                    // Always attempt I/O, including readable bytes delivered with
                    // hangup. recv/send determines EOF or the concrete error.
                    return true;
                }
            }
        }
        return false;
    }, stopped);
    stats.readiness_wait_count = waits;
    if (wait_error) stats.error = std::move(wait_error);
    return stats;
}

struct RegistrationGuard {
    Poller& poller;
    RegistrationToken token;
    ~RegistrationGuard() { (void)poller.unwatch(token); }
};
} // namespace

EchoSessionStats run_echo_session(StreamChannel& channel, const EchoConfig& config) {
    // Controlled test channels have no native descriptor. The bounded retry
    // adapter is retained only for deterministic partial-I/O regression tests.
    return echo(channel, config, [&](Interest, Deadline deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        std::this_thread::sleep_for(std::min(config.wait_retry_delay,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        return true;
    }, [] { return false; });
}

EchoSessionStats run_echo_session(Socket& socket, const EchoConfig& config) {
    auto configured = socket.set_nonblocking(true);
    if (!configured) { EchoSessionStats stats; stats.error = configured.error(); return stats; }
    auto created = Poller::create();
    if (!created) { EchoSessionStats stats; stats.error = created.error(); return stats; }
    auto poller = std::move(created.value());
    auto watched = poller.watch(socket, Interest::read, {1, 1});
    if (!watched) { EchoSessionStats stats; stats.error = watched.error(); return stats; }
    RegistrationGuard guard{poller, watched.value()};
    return ready_echo(socket, poller, watched.value(), config, [] { return false; });
}

EchoServer::EchoServer(EchoConfig config) : config_(std::move(config)) {}
EchoServer::~EchoServer() {
    // The caller must stop and join its driver before destroying the server.
    stop();
    close_listener();
}

void EchoServer::close_listener() noexcept {
    if (listener_token_ && poller_) (void)poller_->unwatch(*listener_token_);
    listener_token_.reset();
    listener_.close();
}

core::Result<void> EchoServer::start() {
    std::lock_guard lock(lifecycle_mutex_);
    return start_locked();
}

core::Result<void> EchoServer::start_locked() {
    if (driving_ || running_.load()) {
        return core::make_error(core::ErrorCategory::invalid_argument, "EchoServer::start", 0, "Server is already started");
    }
    if (config_.buffer_capacity == 0 || config_.buffer_capacity > 16 * 1024 * 1024 ||
        config_.retry_timeout <= std::chrono::milliseconds::zero()) {
        return core::make_error(core::ErrorCategory::invalid_argument, "EchoServer::start", 0, "Invalid buffer or timeout limit");
    }
    close_listener();
    auto created_poller = Poller::create();
    if (!created_poller) return created_poller.error();
    poller_ = std::make_unique<Poller>(std::move(created_poller.value()));
    auto created = create_tcp_socket(config_.endpoint.family());
    if (!created) return created.error();
    auto listener = std::move(created.value());
#ifndef _WIN32
    // On Winsock SO_REUSEADDR would allow another listener to hijack this port.
    auto reuse = listener.set_reuse_address(true);
    if (!reuse) return reuse.error();
#endif
    auto bound = listener.bind(config_.endpoint);
    if (!bound) return bound.error();
    auto listening = listener.listen(128);
    if (!listening) return listening.error();
    auto configured = listener.set_nonblocking(true);
    if (!configured) return configured.error();
    auto endpoint = listener.local_endpoint();
    if (!endpoint) return endpoint.error();
    auto watched = poller_->watch(listener, Interest::read, {0, ++connection_generation_});
    if (!watched) return watched.error();
    listener_token_ = watched.value();
    actual_endpoint_ = endpoint.value();
    listener_ = std::move(listener);
    stop_requested_ = false;
    running_.store(true);
    return {};
}

void EchoServer::stop() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    stop_requested_ = true;
    running_.store(false);
    if (poller_) (void)poller_->wake();
    if (!driving_) close_listener();
}

bool EchoServer::is_running() const noexcept { return running_.load(); }

core::Result<void> EchoServer::enter_driver(bool auto_start) {
    std::lock_guard lock(lifecycle_mutex_);
    if (driving_) return core::make_error(core::ErrorCategory::invalid_argument, "echo driver", 0, "Only one driver may run");
    // Auto-start and stop are serialized. A shutdown just before run() must not
    // silently start another listener after the shutdown thread has returned.
    if (auto_start && !running_.load() && !stop_requested_) {
        auto started = start_locked();
        if (!started) return started;
    }
    if (!running_.load()) return core::make_error(core::ErrorCategory::not_connected, "echo driver", 0, "Server is stopped");
    driving_ = true;
    return {};
}
void EchoServer::leave_driver() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    driving_ = false;
    if (!running_.load()) close_listener();
}

core::Result<void> EchoServer::run() {
    auto entered = enter_driver(true);
    if (!entered) {
        if (entered.error().category == core::ErrorCategory::not_connected) return {};
        return entered;
    }
    core::Result<void> result;
    try {
        while (running_.load()) {
            result = accept_one();
            if (!result) { running_.store(false); break; }
        }
    } catch (...) {
        running_.store(false);
        leave_driver();
        throw;
    }
    leave_driver();
    return result;
}

core::Result<void> EchoServer::accept_and_handle_one() {
    auto entered = enter_driver();
    if (!entered) return entered;
    try {
        auto result = accept_one();
        leave_driver();
        return result;
    } catch (...) {
        running_.store(false);
        leave_driver();
        throw;
    }
}

core::Result<void> EchoServer::accept_one() {
    while (running_.load()) {
        auto ready = poller_->wait(Deadline::max());
        if (!ready) return ready.error();
        if (!running_.load()) return {};
        bool readable = false;
        for (const auto& event : ready.value()) {
            if (event.token == listener_token_ && poller_->is_current(event)) {
                if (event.error || event.hangup) return core::make_error(core::ErrorCategory::io_error, "listener readiness");
                readable = event.readable;
            }
        }
        if (!readable) continue;
        auto accepted = listener_.accept();
        if (!accepted) {
            if (accepted.error().category == core::ErrorCategory::would_block ||
                accepted.error().category == core::ErrorCategory::interrupted ||
                accepted.error().category == core::ErrorCategory::connection_aborted) continue;
            return accepted.error();
        }
        Socket client = std::move(accepted.value());
        auto configured = client.set_nonblocking(true);
        if (!configured) return configured.error();
        auto watched = poller_->watch(client, Interest::read, {1, ++connection_generation_});
        if (!watched) return watched.error();
        // Suppress listener readiness while handling this single client; queued
        // accepts otherwise keep the wait runnable while the client is idle.
        auto paused = poller_->modify(*listener_token_, Interest::none);
        if (!paused) { (void)poller_->unwatch(watched.value()); return paused.error(); }
        {
            RegistrationGuard guard{*poller_, watched.value()};
            (void)ready_echo(client, *poller_, watched.value(), config_, [&] { return !running_.load(); });
        }
        client.close(); // its registration was removed by the guard first
        auto resumed = poller_->modify(*listener_token_, Interest::read);
        if (!resumed) return resumed;
        return {};
    }
    return {};
}
} // namespace net
