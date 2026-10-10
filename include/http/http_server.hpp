#pragma once

#include "core/result.hpp"
#include "http/http_session.hpp"
#include "net/endpoint.hpp"
#include "net/socket.hpp"

#include <atomic>

namespace http {

struct HttpServerConfig {
    net::Endpoint endpoint{net::Endpoint::ipv4_loopback(8080)};
    HttpSessionConfig session_config{};
};

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
    HttpServerConfig config_;
    net::Socket listener_;
    std::atomic<bool> running_{false};
    net::Endpoint actual_endpoint_;
};

} // namespace http