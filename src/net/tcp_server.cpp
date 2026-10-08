#include "net/tcp_server.hpp"

#include <thread>

namespace net {

TcpServer::TcpServer(
    std::uint16_t port,
    ConnectionHandler handler
)
    : endpoint_(Endpoint::ipv4_loopback(port)),
      handler_(std::move(handler)) {}

TcpServer::TcpServer(
    Endpoint endpoint,
    ConnectionHandler handler
)
    : endpoint_(std::move(endpoint)),
      handler_(std::move(handler)) {}

core::Result<void> TcpServer::run() {
    auto sock_res = create_tcp_socket(endpoint_.family());
    if (!sock_res) {
        return sock_res.error();
    }
    server_socket_ = std::move(sock_res.value());

    auto reuse_res = server_socket_.set_reuse_address(true);
    if (!reuse_res) {
        return reuse_res.error();
    }

    auto bind_res = server_socket_.bind(endpoint_);
    if (!bind_res) {
        return bind_res.error();
    }

    auto listen_res = server_socket_.listen(128);
    if (!listen_res) {
        return listen_res.error();
    }

    running_ = true;

    while (running_) {
        auto client_res = server_socket_.accept();
        if (!client_res) {
            if (!running_) {
                break;
            }
            continue;
        }

        Socket client = std::move(client_res.value());
        std::thread(
            [handler = handler_](Socket socket) {
                handler(std::move(socket));
            },
            std::move(client)
        ).detach();
    }

    return core::Result<void>::success();
}

void TcpServer::stop() noexcept {
    running_ = false;
    server_socket_.close();
}

} // namespace net