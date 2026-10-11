#include "http/http_server.hpp"

#include "concurrency/single_session_driver.hpp"
#include "http/serializer.hpp"

#include <algorithm>
#include <utility>

namespace http {
namespace {
struct RegistrationGuard {
    net::Poller& poller;
    net::RegistrationToken token;
    ~RegistrationGuard() { (void)poller.unwatch(token); }
};
} // namespace

using net::Deadline;
using net::Interest;
using net::Poller;
using net::Socket;
using net::create_tcp_socket;

HttpServer::HttpServer(HttpServerConfig config) : config_(std::move(config)) {}
HttpServer::~HttpServer() {
    // The caller must stop and join its driver before destroying the server.
    stop();
    close_listener();
}

void HttpServer::close_listener() noexcept {
    if (listener_token_ && poller_) (void)poller_->unwatch(*listener_token_);
    listener_token_.reset();
    listener_.close();
}

core::Result<void> HttpServer::start() {
    std::lock_guard lock(lifecycle_mutex_);
    return start_locked();
}

core::Result<void> HttpServer::start_locked() {
    if (driving_ || running_.load()) {
        return core::make_error(core::ErrorCategory::invalid_argument, "HttpServer::start", 0, "Server is already started");
    }
    auto response = make_fixed_response(config_.session_config);
    if (!response) return response.error();
    auto wire = serialize_response(response.value(),
        {std::min<std::size_t>(8192, config_.session_config.max_output_bytes),
         config_.session_config.max_output_bytes});
    if (!wire) return wire.error();
    if (wire.value().size() > config_.session_config.max_output_bytes) {
        return core::make_error(core::ErrorCategory::invalid_argument, "HttpServer::start", 0, "Output limit is too small");
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

void HttpServer::stop() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    stop_requested_ = true;
    running_.store(false);
    if (poller_) (void)poller_->wake();
    if (!driving_) close_listener();
}

bool HttpServer::is_running() const noexcept { return running_.load(); }

core::Result<void> HttpServer::enter_driver(bool auto_start) {
    std::lock_guard lock(lifecycle_mutex_);
    if (driving_) return core::make_error(core::ErrorCategory::invalid_argument, "HTTP driver", 0, "Only one driver may run");
    // Auto-start and stop are serialized. A shutdown just before run() must not
    // silently start another listener after the shutdown thread has returned.
    if (auto_start && !running_.load() && !stop_requested_) {
        auto started = start_locked();
        if (!started) return started;
    }
    if (!running_.load()) return core::make_error(core::ErrorCategory::not_connected, "HTTP driver", 0, "Server is stopped");
    driving_ = true;
    return {};
}
void HttpServer::leave_driver() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    driving_ = false;
    if (!running_.load()) close_listener();
}

core::Result<void> HttpServer::run() {
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

core::Result<void> HttpServer::accept_and_handle_one() {
    auto entered = enter_driver();
    if (!entered) return entered;
    try {
        auto result = accept_one();
        if (!result) running_.store(false);
        leave_driver();
        return result;
    } catch (...) {
        running_.store(false);
        leave_driver();
        throw;
    }
}

core::Result<void> HttpServer::accept_one() {
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
            HttpSession session(config_.session_config, {1, connection_generation_});
            concurrency::SingleSessionDriver driver;
            // A client error ends its session; the listener remains available.
            (void)driver.drive(client, session, *poller_, watched.value(),
                               [&] { return !running_.load(); });
        }
        client.close(); // its registration was removed by the guard first
        auto resumed = poller_->modify(*listener_token_, Interest::read);
        if (!resumed) return resumed;
        return {};
    }
    return {};
}
} // namespace http
