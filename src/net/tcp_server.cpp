#include "net/tcp_server.hpp"

#include <stdexcept>
#include <system_error>

namespace net {

TcpServer::TcpServer(
    std::uint16_t port,
    ConnectionHandler handler
)
    : port_(port),
      handler_(std::move(handler)) {}

void TcpServer::run() {

    std::error_code ec;

    server_socket_ = Socket(
        static_cast<std::intptr_t>(
#ifdef _WIN32
            ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
#else
            ::socket(AF_INET, SOCK_STREAM, 0)
#endif
        )
    );

    if (!server_socket_.valid()) {
        throw std::runtime_error(
            "Failed to create TCP socket"
        );
    }

    server_socket_.set_reuse_address(true, ec);

    if (ec) {
        throw std::system_error(ec);
    }

    server_socket_.bind(port_, ec);

    if (ec) {
        throw std::system_error(ec);
    }

    server_socket_.listen(128, ec);

    if (ec) {
        throw std::system_error(ec);
    }

    running_ = true;

    while (running_) {

        Socket client =
            server_socket_.accept(ec);

        if (ec) {
            if (running_) {
                continue;
            }

            break;
        }

        std::thread(
            [handler = handler_](Socket socket) {

                handler(std::move(socket));

            },
            std::move(client)
        ).detach();
    }
}

void TcpServer::stop() noexcept {

    running_ = false;

    server_socket_.close();
}

}