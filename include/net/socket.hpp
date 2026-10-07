#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace net {

class Socket {
public:
    Socket() = default;

    explicit Socket(std::intptr_t native_handle) noexcept
        : handle_(native_handle) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    ~Socket();

    [[nodiscard]]
    bool valid() const noexcept;

    void close() noexcept;

    [[nodiscard]]
    std::size_t send(
        std::string_view data,
        std::error_code& ec
    ) noexcept;

    [[nodiscard]]
    std::size_t receive(
        char* buffer,
        std::size_t size,
        std::error_code& ec
    ) noexcept;

    [[nodiscard]]
    Socket accept(std::error_code& ec) noexcept;

    void bind(
        std::uint16_t port,
        std::error_code& ec
    ) noexcept;

    void listen(
        int backlog,
        std::error_code& ec
    ) noexcept;

    void set_reuse_address(
        bool enabled,
        std::error_code& ec
    ) noexcept;

private:
    std::intptr_t handle_{-1};

    friend class TcpServer;
};

}