#pragma once

#include "core/config.hpp"
#include "http/router.hpp"
#include "net/endpoint.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace application {

struct ServerOptions {
    core::DriverMode driver{core::DriverMode::thread_pool};
    std::size_t workers{4};
    // Active session limit for the event loop; pool concurrency is workers
    // (which must not exceed max_sessions). The single driver serves one client.
    std::size_t max_sessions{1024};
    std::size_t accept_queue{128}; // Pool sockets waiting for a worker.
    std::size_t max_requests_per_connection{100};
    core::BufferConfig buffers{};
    std::chrono::milliseconds idle_timeout{5000};
    std::chrono::milliseconds progress_timeout{5000};
    std::chrono::milliseconds drain_timeout{2000};
};

// Owns network initialization, immutable routes and the existing runtime/drivers.
// Configure, listen and wait on the control thread. After listen succeeds, only
// request_stop() may be called concurrently. References captured by handlers must
// outlive the server. Handlers run on session threads; event-loop handlers must not block.
class Server {
public:
    explicit Server(ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] core::Result<void> get(std::string path, http::RouteHandler handler);
    // Starts in the background. Port 0 selects an available port. Returns the
    // actual bound endpoint; failed startup can be retried on this instance.
    // After successful startup routes are sealed; use a new Server to restart.
    [[nodiscard]] core::Result<net::Endpoint> listen(std::uint32_t port, std::string bind_address = "127.0.0.1");
    // Call from a control thread, not from inside a route handler.
    void request_stop() noexcept;
    // Blocks until stop is requested and drivers/helpers finish. No signal hooks
    // are installed by this library; the executable owns process signal policy.
    void wait() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace application
