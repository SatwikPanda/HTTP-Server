#pragma once

#include "core/result.hpp"
#include "http/http_session.hpp"
#include "net/endpoint.hpp"
#include "net/poller.hpp"
#include "net/socket.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace http {

struct HttpServerConfig {
    net::Endpoint endpoint{net::Endpoint::ipv4_loopback(8080)};
    HttpSessionConfig session_config{};
};

// One driver owns socket operations. stop() and is_running() are thread-safe.
// Stop and join that driver before destruction; NetworkRuntime must outlive this
// server. Read local_endpoint() after start(), before another start() changes it.
class HttpServer {
public:
    explicit HttpServer(HttpServerConfig config = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] core::Result<void> start();
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    // Run synchronous accept loop until stop() is called or error occurs
    [[nodiscard]] core::Result<void> run();

    // Accept and handle a single client session
    [[nodiscard]] core::Result<void> accept_and_handle_one();

    [[nodiscard]] const net::Endpoint& local_endpoint() const noexcept {
        return actual_endpoint_;
    }

private:
    [[nodiscard]] core::Result<void> start_locked();
    [[nodiscard]] core::Result<void> enter_driver(bool auto_start = false);
    [[nodiscard]] core::Result<void> accept_one();
    void leave_driver() noexcept;
    void close_listener() noexcept;
    HttpServerConfig config_;
    net::Socket listener_;
    std::unique_ptr<net::Poller> poller_;
    std::optional<net::RegistrationToken> listener_token_;
    std::mutex lifecycle_mutex_;
    bool driving_{false};
    bool stop_requested_{false};
    std::atomic<bool> running_{false};
    std::uint64_t connection_generation_{0};
    net::Endpoint actual_endpoint_;
};

} // namespace http
