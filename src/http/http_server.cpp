#include "http/http_server.hpp"

namespace http {

HttpServer::HttpServer(HttpServerConfig config)
    : config_(std::move(config)) {}

HttpServer::~HttpServer() {
    stop();
}

core::Result<void> HttpServer::start() {
    auto sock_res = net::create_tcp_socket(config_.endpoint.family());
    if (!sock_res) {
        return sock_res.error();
    }
    listener_ = std::move(sock_res.value());

    auto reuse_res = listener_.set_reuse_address(true);
    if (!reuse_res) {
        listener_.close();
        return reuse_res.error();
    }

    auto bind_res = listener_.bind(config_.endpoint);
    if (!bind_res) {
        listener_.close();
        return bind_res.error();
    }

    auto listen_res = listener_.listen(128);
    if (!listen_res) {
        listener_.close();
        return listen_res.error();
    }

    auto local_res = listener_.local_endpoint();
    if (local_res) {
        actual_endpoint_ = local_res.value();
    } else {
        actual_endpoint_ = config_.endpoint;
    }

    running_ = true;
    return core::Result<void>::success();
}

void HttpServer::stop() noexcept {
    running_ = false;
    listener_.close();
}

bool HttpServer::is_running() const noexcept {
    return running_ && listener_.is_valid();
}

core::Result<void> HttpServer::run() {
    if (!is_running()) {
        auto start_res = start();
        if (!start_res) {
            return start_res;
        }
    }

    while (running_) {
        auto handle_res = accept_and_handle_one();
        if (!handle_res) {
            if (!running_) {
                break;
            }
            if (handle_res.error().category == core::ErrorCategory::interrupted ||
                handle_res.error().category == core::ErrorCategory::would_block) {
                continue;
            }
            return handle_res;
        }
    }

    return core::Result<void>::success();
}

core::Result<void> HttpServer::accept_and_handle_one() {
    auto client_res = listener_.accept();
    if (!client_res) {
        if (!running_) {
            return core::Result<void>::success();
        }
        return client_res.error();
    }

    net::Socket client = std::move(client_res.value());

    auto nonblocking_res = client.set_nonblocking(true);
    if (!nonblocking_res) {
        client.close();
        return nonblocking_res.error();
    }

    run_http_session(client, config_.session_config);

    client.close();
    return core::Result<void>::success();
}

} // namespace http