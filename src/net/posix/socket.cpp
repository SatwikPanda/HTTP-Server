#ifndef _WIN32

#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <string>

namespace net {

namespace {

core::ErrorCategory map_posix_error(int err) noexcept {
    switch (err) {
        case 0:
            return core::ErrorCategory::none;
        case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
        case EWOULDBLOCK:
#endif
            return core::ErrorCategory::would_block;
        case EINTR:
            return core::ErrorCategory::interrupted;
        case ECONNREFUSED:
            return core::ErrorCategory::connection_refused;
        case ECONNRESET:
            return core::ErrorCategory::connection_reset;
        case ECONNABORTED:
            return core::ErrorCategory::connection_aborted;
        case ENOTCONN:
            return core::ErrorCategory::not_connected;
        case EISCONN:
            return core::ErrorCategory::already_connected;
        case EADDRINUSE:
            return core::ErrorCategory::address_in_use;
        case EADDRNOTAVAIL:
            return core::ErrorCategory::address_not_available;
        case ENETUNREACH:
            return core::ErrorCategory::network_unreachable;
        case EHOSTUNREACH:
            return core::ErrorCategory::host_unreachable;
        case ETIMEDOUT:
            return core::ErrorCategory::timed_out;
        case EINVAL:
            return core::ErrorCategory::invalid_argument;
        case EBADF:
        case ENOTSOCK:
            return core::ErrorCategory::bad_descriptor;
        case EPIPE:
            return core::ErrorCategory::connection_reset;
        default:
            return core::ErrorCategory::io_error;
    }
}

std::string get_posix_error_message(int err) {
    char buf[256]{};
#if (_POSIX_C_SOURCE >= 200112L) && ! _GNU_SOURCE
    if (strerror_r(err, buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
#elif defined(_GNU_SOURCE)
    char* msg = strerror_r(err, buf, sizeof(buf));
    if (msg != nullptr) {
        return std::string(msg);
    }
#else
    std::strncpy(buf, std::strerror(err), sizeof(buf) - 1);
    return std::string(buf);
#endif
    return "POSIX error " + std::to_string(err);
}

core::Error make_posix_error(std::string operation, int err) {
    return core::make_error(
        map_posix_error(err),
        std::move(operation),
        err,
        get_posix_error_message(err)
    );
}

bool endpoint_to_sockaddr(
    const Endpoint& ep,
    sockaddr_storage& out_addr,
    socklen_t& out_len,
    core::Error& out_error
) {
    std::memset(&out_addr, 0, sizeof(out_addr));

    if (ep.family() == AddressFamily::ipv6) {
        auto* addr6 = reinterpret_cast<sockaddr_in6*>(&out_addr);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(ep.port());
        if (::inet_pton(AF_INET6, ep.address().c_str(), &addr6->sin6_addr) != 1) {
            int err = errno;
            out_error = make_posix_error("inet_pton(IPv6)", err);
            return false;
        }
        out_len = sizeof(sockaddr_in6);
        return true;
    }

    auto* addr4 = reinterpret_cast<sockaddr_in*>(&out_addr);
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(ep.port());
    if (::inet_pton(AF_INET, ep.address().c_str(), &addr4->sin_addr) != 1) {
        int err = errno;
        out_error = make_posix_error("inet_pton(IPv4)", err);
        return false;
    }
    out_len = sizeof(sockaddr_in);
    return true;
}

core::Result<Endpoint> sockaddr_to_endpoint(const sockaddr_storage& addr, socklen_t len) {
    if (addr.ss_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        char ip_buf[INET6_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET6, &addr6->sin6_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            int err = errno;
            return make_posix_error("inet_ntop(IPv6)", err);
        }
        return Endpoint(AddressFamily::ipv6, std::string(ip_buf), ntohs(addr6->sin6_port));
    }

    if (addr.ss_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        const auto* addr4 = reinterpret_cast<const sockaddr_in*>(&addr);
        char ip_buf[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &addr4->sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            int err = errno;
            return make_posix_error("inet_ntop(IPv4)", err);
        }
        return Endpoint(AddressFamily::ipv4, std::string(ip_buf), ntohs(addr4->sin_port));
    }

    return core::make_error(
        core::ErrorCategory::invalid_argument,
        "sockaddr_to_endpoint",
        0,
        "Unsupported address family"
    );
}

constexpr std::size_t kMaxChunkSize = 1024 * 1024; // Cap native buffer length to 1 MB

} // namespace

class Socket::Impl {
public:
    int handle_{-1};

    Impl() = default;
    explicit Impl(int h) noexcept : handle_(h) {}

    ~Impl() {
        close();
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return handle_ >= 0;
    }

    void close() noexcept {
        if (handle_ >= 0) {
            int h = handle_;
            handle_ = -1;
            ::close(h);
        }
    }
};

Socket::Socket() noexcept = default;

Socket::Socket(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Socket::Socket(Socket&& other) noexcept
    : impl_(std::move(other.impl_)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Socket::~Socket() {
    close();
}

bool Socket::is_valid() const noexcept {
    return impl_ != nullptr && impl_->is_valid();
}

void Socket::close() noexcept {
    if (impl_) {
        impl_->close();
        impl_.reset();
    }
}

core::Result<void> Socket::bind(const Endpoint& local) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "bind", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    core::Error conv_err;
    if (!endpoint_to_sockaddr(local, addr, addr_len, conv_err)) {
        return conv_err;
    }

    if (::bind(impl_->handle_, reinterpret_cast<const sockaddr*>(&addr), addr_len) < 0) {
        int err = errno;
        return make_posix_error("bind", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::listen(int backlog) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "listen", 0, "Socket is not valid");
    }

    if (::listen(impl_->handle_, backlog) < 0) {
        int err = errno;
        return make_posix_error("listen", err);
    }
    return core::Result<void>::success();
}

core::Result<Socket> Socket::accept() {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "accept", 0, "Socket is not valid");
    }

    sockaddr_storage client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    int client_handle = ::accept(
        impl_->handle_,
        reinterpret_cast<sockaddr*>(&client_addr),
        &addr_len
    );

    if (client_handle < 0) {
        int err = errno;
        return make_posix_error("accept", err);
    }

#if defined(SO_NOSIGPIPE)
    int opt = 1;
    ::setsockopt(client_handle, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    return Socket(std::make_unique<Impl>(client_handle));
}

core::Result<void> Socket::connect(const Endpoint& remote) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "connect", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    core::Error conv_err;
    if (!endpoint_to_sockaddr(remote, addr, addr_len, conv_err)) {
        return conv_err;
    }

    if (::connect(impl_->handle_, reinterpret_cast<const sockaddr*>(&addr), addr_len) < 0) {
        int err = errno;
        return make_posix_error("connect", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::set_nonblocking(bool enabled) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "set_nonblocking", 0, "Socket is not valid");
    }

    int flags = ::fcntl(impl_->handle_, F_GETFL, 0);
    if (flags < 0) {
        int err = errno;
        return make_posix_error("fcntl(F_GETFL)", err);
    }

    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(impl_->handle_, F_SETFL, flags) < 0) {
        int err = errno;
        return make_posix_error("fcntl(F_SETFL)", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::set_reuse_address(bool enabled) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "set_reuse_address", 0, "Socket is not valid");
    }

    int val = enabled ? 1 : 0;
    if (::setsockopt(impl_->handle_, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
        int err = errno;
        return make_posix_error("setsockopt(SO_REUSEADDR)", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::shutdown_write() {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "shutdown_write", 0, "Socket is not valid");
    }

    if (::shutdown(impl_->handle_, SHUT_WR) < 0) {
        int err = errno;
        return make_posix_error("shutdown(SHUT_WR)", err);
    }
    return core::Result<void>::success();
}

ReadResult Socket::read_some(std::span<std::byte> destination) {
    if (!is_valid()) {
        return ReadResult::failure(
            core::make_error(core::ErrorCategory::bad_descriptor, "read_some", 0, "Socket is not valid")
        );
    }

    if (destination.empty()) {
        return ReadResult::success(0);
    }

    std::size_t to_read = std::min<std::size_t>(destination.size(), kMaxChunkSize);

    while (true) {
        ssize_t res = ::recv(
            impl_->handle_,
            destination.data(),
            to_read,
            0
        );

        if (res < 0) {
            int err = errno;
            if (err == EINTR) {
                continue; // Retry interrupted native operation
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                return ReadResult::would_block_result();
            }
            return ReadResult::failure(make_posix_error("recv", err));
        }

        if (res == 0) {
            return ReadResult::eof();
        }

        return ReadResult::success(static_cast<std::size_t>(res));
    }
}

WriteResult Socket::write_some(std::span<const std::byte> source) {
    if (!is_valid()) {
        return WriteResult::failure(
            core::make_error(core::ErrorCategory::bad_descriptor, "write_some", 0, "Socket is not valid")
        );
    }

    if (source.empty()) {
        return WriteResult::success(0);
    }

    std::size_t to_write = std::min<std::size_t>(source.size(), kMaxChunkSize);

    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif

    while (true) {
        ssize_t res = ::send(
            impl_->handle_,
            source.data(),
            to_write,
            flags
        );

        if (res < 0) {
            int err = errno;
            if (err == EINTR) {
                continue; // Retry interrupted native operation
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                return WriteResult::would_block_result();
            }
            return WriteResult::failure(make_posix_error("send", err));
        }

        return WriteResult::success(static_cast<std::size_t>(res));
    }
}

core::Result<Endpoint> Socket::local_endpoint() const {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "local_endpoint", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(impl_->handle_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        int err = errno;
        return make_posix_error("getsockname", err);
    }

    return sockaddr_to_endpoint(addr, len);
}

core::Result<Endpoint> Socket::remote_endpoint() const {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "remote_endpoint", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(impl_->handle_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        int err = errno;
        return make_posix_error("getpeername", err);
    }

    return sockaddr_to_endpoint(addr, len);
}

// Free functions
core::Result<Socket> create_tcp_socket(AddressFamily family) {
    int af = (family == AddressFamily::ipv6) ? AF_INET6 : AF_INET;
    int h = ::socket(af, SOCK_STREAM, 0);
    if (h < 0) {
        int err = errno;
        return make_posix_error("socket", err);
    }

#if defined(SO_NOSIGPIPE)
    int opt = 1;
    ::setsockopt(h, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    return Socket(std::make_unique<Socket::Impl>(h));
}

core::Result<void> bind(Socket& socket, const Endpoint& local) {
    return socket.bind(local);
}

core::Result<void> listen(Socket& socket, int backlog) {
    return socket.listen(backlog);
}

core::Result<Socket> accept(Socket& listener) {
    return listener.accept();
}

core::Result<void> connect(Socket& socket, const Endpoint& remote) {
    return socket.connect(remote);
}

core::Result<void> set_nonblocking(Socket& socket, bool enabled) {
    return socket.set_nonblocking(enabled);
}

core::Result<void> set_reuse_address(Socket& socket, bool enabled) {
    return socket.set_reuse_address(enabled);
}

core::Result<void> shutdown_write(Socket& socket) {
    return socket.shutdown_write();
}

void close(Socket& socket) noexcept {
    socket.close();
}

ReadResult read_some(Socket& socket, std::span<std::byte> destination) {
    return socket.read_some(destination);
}

WriteResult write_some(Socket& socket, std::span<const std::byte> source) {
    return socket.write_some(source);
}

} // namespace net

#endif
