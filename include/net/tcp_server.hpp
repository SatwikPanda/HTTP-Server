#pragma once

#include "net/endpoint.hpp"
#include "net/socket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace net {

class TcpServer {
public:
    using ConnectionHandler = std::function<void(Socket)>;

    TcpServer(
        std::uint16_t port,
        ConnectionHandler handler
    );

    TcpServer(
        Endpoint endpoint,
        ConnectionHandler handler
    );

    core::Result<void> run();

    void stop() noexcept;

private:
    Endpoint endpoint_;
    ConnectionHandler handler_;
    Socket server_socket_;
    std::atomic<bool> running_{false};
};

} // namespace net