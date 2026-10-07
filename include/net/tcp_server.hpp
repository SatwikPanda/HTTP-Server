#pragma once

#include "net/socket.hpp"

#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace net {

class TcpServer {
public:

    using ConnectionHandler =
        std::function<void(Socket)>;

    TcpServer(
        std::uint16_t port,
        ConnectionHandler handler
    );

    void run();

    void stop() noexcept;

private:

    std::uint16_t port_;

    ConnectionHandler handler_;

    Socket server_socket_;

    bool running_{false};
};

}