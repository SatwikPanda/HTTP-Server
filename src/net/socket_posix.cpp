#ifndef _WIN32

#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int to_native(std::intptr_t handle) {
    return static_cast<int>(handle);
}

std::intptr_t from_native(int handle) {
    return static_cast<std::intptr_t>(handle);
}

}

namespace net {

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();

        handle_ = other.handle_;
        other.handle_ = -1;
    }

    return *this;
}

Socket::~Socket() {
    close();
}

bool Socket::valid() const noexcept {
    return handle_ != -1;
}

void Socket::close() noexcept {
    if (valid()) {
        ::close(to_native(handle_));
        handle_ = -1;
    }
}

std::size_t Socket::send(
    std::string_view data,
    std::error_code& ec
) noexcept {

    ec.clear();

    ssize_t result = ::send(
        to_native(handle_),
        data.data(),
        data.size(),
        MSG_NOSIGNAL
    );

    if (result < 0) {
        ec = std::error_code(errno, std::generic_category());
        return 0;
    }

    return static_cast<std::size_t>(result);
}

std::size_t Socket::receive(
    char* buffer,
    std::size_t size,
    std::error_code& ec
) noexcept {

    ec.clear();

    ssize_t result = ::recv(
        to_native(handle_),
        buffer,
        size,
        0
    );

    if (result < 0) {
        ec = std::error_code(errno, std::generic_category());
        return 0;
    }

    return static_cast<std::size_t>(result);
}

Socket Socket::accept(std::error_code& ec) noexcept {

    ec.clear();

    int client = ::accept(
        to_native(handle_),
        nullptr,
        nullptr
    );

    if (client < 0) {
        ec = std::error_code(errno, std::generic_category());
        return {};
    }

    return Socket(from_native(client));
}

void Socket::bind(
    std::uint16_t port,
    std::error_code& ec
) noexcept {

    ec.clear();

    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(
        to_native(handle_),
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    ) < 0) {

        ec = std::error_code(errno, std::generic_category());
    }
}

void Socket::listen(
    int backlog,
    std::error_code& ec
) noexcept {

    ec.clear();

    if (::listen(to_native(handle_), backlog) < 0) {
        ec = std::error_code(errno, std::generic_category());
    }
}

void Socket::set_reuse_address(
    bool enabled,
    std::error_code& ec
) noexcept {

    ec.clear();

    int value = enabled ? 1 : 0;

    if (::setsockopt(
        to_native(handle_),
        SOL_SOCKET,
        SO_REUSEADDR,
        &value,
        sizeof(value)
    ) < 0) {

        ec = std::error_code(errno, std::generic_category());
    }
}

}

#endif