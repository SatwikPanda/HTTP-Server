#ifdef _WIN32

#include "net/socket.hpp"
#include "../detail/socket_access.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

namespace net {

namespace {

core::ErrorCategory map_wsa_error(int err) noexcept {
    switch (err) {
        case 0:
            return core::ErrorCategory::none;
        case WSAEWOULDBLOCK:
            return core::ErrorCategory::would_block;
        case WSAEINTR:
            return core::ErrorCategory::interrupted;
        case WSAECONNREFUSED:
            return core::ErrorCategory::connection_refused;
        case WSAECONNRESET:
            return core::ErrorCategory::connection_reset;
        case WSAECONNABORTED:
            return core::ErrorCategory::connection_aborted;
        case WSAENOTCONN:
            return core::ErrorCategory::not_connected;
        case WSAEISCONN:
            return core::ErrorCategory::already_connected;
        case WSAEADDRINUSE:
            return core::ErrorCategory::address_in_use;
        case WSAEADDRNOTAVAIL:
            return core::ErrorCategory::address_not_available;
        case WSAENETUNREACH:
            return core::ErrorCategory::network_unreachable;
        case WSAEHOSTUNREACH:
            return core::ErrorCategory::host_unreachable;
        case WSAETIMEDOUT:
            return core::ErrorCategory::timed_out;
        case WSAEINVAL:
            return core::ErrorCategory::invalid_argument;
        case WSAEBADF:
        case WSAENOTSOCK:
            return core::ErrorCategory::bad_descriptor;
        default:
            return core::ErrorCategory::io_error;
    }
}

std::string get_wsa_error_message(int err) {
    char* buffer = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(err),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr
    );

    if (len > 0 && buffer != nullptr) {
        std::string msg(buffer, len);
        LocalFree(buffer);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) {
            msg.pop_back();
        }
        return msg;
    }
    return "Winsock error " + std::to_string(err);
}

core::Error make_wsa_error(std::string operation, int err) {
    return core::make_error(
        map_wsa_error(err),
        std::move(operation),
        err,
        get_wsa_error_message(err)
    );
}

bool endpoint_to_sockaddr(
    const Endpoint& ep,
    sockaddr_storage& out_addr,
    int& out_len,
    core::Error& out_error
) {
    std::memset(&out_addr, 0, sizeof(out_addr));
    if ((ep.family() != AddressFamily::ipv4 && ep.family() != AddressFamily::ipv6) ||
        ep.address().find('\0') != std::string::npos) {
        out_error = core::make_error(core::ErrorCategory::invalid_argument, "endpoint", 0, "Invalid address family or embedded NUL");
        return false;
    }

    if (ep.family() == AddressFamily::ipv6) {
        auto* addr6 = reinterpret_cast<sockaddr_in6*>(&out_addr);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(ep.port());
        if (::inet_pton(AF_INET6, ep.address().c_str(), &addr6->sin6_addr) != 1) {
            out_error = core::make_error(core::ErrorCategory::invalid_argument, "inet_pton(IPv6)", 0, "Invalid numeric IPv6 address");
            return false;
        }
        out_len = sizeof(sockaddr_in6);
        return true;
    }

    auto* addr4 = reinterpret_cast<sockaddr_in*>(&out_addr);
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(ep.port());
    if (::inet_pton(AF_INET, ep.address().c_str(), &addr4->sin_addr) != 1) {
        out_error = core::make_error(core::ErrorCategory::invalid_argument, "inet_pton(IPv4)", 0, "Invalid numeric IPv4 address");
        return false;
    }
    out_len = sizeof(sockaddr_in);
    return true;
}

core::Result<Endpoint> sockaddr_to_endpoint(const sockaddr_storage& addr, int len) {
    if (addr.ss_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
        const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        char ip_buf[INET6_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET6, &addr6->sin6_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            int err = ::WSAGetLastError();
            return make_wsa_error("inet_ntop(IPv6)", err);
        }
        return Endpoint(AddressFamily::ipv6, std::string(ip_buf), ntohs(addr6->sin6_port));
    }

    if (addr.ss_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto* addr4 = reinterpret_cast<const sockaddr_in*>(&addr);
        char ip_buf[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &addr4->sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            int err = ::WSAGetLastError();
            return make_wsa_error("inet_ntop(IPv4)", err);
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
    SOCKET handle_{INVALID_SOCKET};
    std::shared_ptr<detail::SocketLifetime> lifetime_{std::make_shared<detail::SocketLifetime>()};
    enum class Attempt { none, pending, connected, failed };
    Attempt attempt_{Attempt::none};
    int connect_error_{};

    Impl() = default;
    explicit Impl(SOCKET h) noexcept : handle_(h) {}

    ~Impl() {
        close();
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return handle_ != INVALID_SOCKET;
    }

    void close() noexcept {
        lifetime_.reset();
        if (handle_ != INVALID_SOCKET) {
            SOCKET h = handle_;
            handle_ = INVALID_SOCKET;
            ::closesocket(h);
        }
    }
};

std::uintptr_t detail::SocketAccess::handle(const Socket& socket) noexcept {
    return socket.impl_ ? static_cast<std::uintptr_t>(socket.impl_->handle_) : static_cast<std::uintptr_t>(INVALID_SOCKET);
}
std::weak_ptr<detail::SocketLifetime> detail::SocketAccess::lifetime(const Socket& socket) noexcept {
    return socket.impl_ ? socket.impl_->lifetime_ : std::weak_ptr<SocketLifetime>{};
}

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
    int addr_len = 0;
    core::Error conv_err;
    if (!endpoint_to_sockaddr(local, addr, addr_len, conv_err)) {
        return conv_err;
    }

    if (::bind(impl_->handle_, reinterpret_cast<const sockaddr*>(&addr), addr_len) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("bind", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::listen(int backlog) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "listen", 0, "Socket is not valid");
    }

    if (::listen(impl_->handle_, backlog) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("listen", err);
    }
    return core::Result<void>::success();
}

core::Result<Socket> Socket::accept() {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "accept", 0, "Socket is not valid");
    }

    sockaddr_storage client_addr{};
    int addr_len = sizeof(client_addr);

    SOCKET client_handle;
    for (;;) {
        addr_len = sizeof(client_addr);
        client_handle = ::accept(impl_->handle_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_handle != INVALID_SOCKET) break;
        const int err = ::WSAGetLastError();
        if (err == WSAEINTR) continue;
        return make_wsa_error("accept", err);
    }
    try {
        return Socket(std::make_unique<Impl>(client_handle));
    } catch (...) {
        ::closesocket(client_handle);
        throw;
    }
}

core::Result<void> Socket::connect(const Endpoint& remote) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "connect", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    int addr_len = 0;
    core::Error conv_err;
    if (!endpoint_to_sockaddr(remote, addr, addr_len, conv_err)) {
        return conv_err;
    }

    if (::connect(impl_->handle_, reinterpret_cast<const sockaddr*>(&addr), addr_len) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("connect", err);
    }
    return core::Result<void>::success();
}

core::Result<ConnectState> Socket::begin_connect(const Endpoint& remote) {
    if (!is_valid()) return make_wsa_error("begin_connect", WSAENOTSOCK);
    if (impl_->attempt_ != Impl::Attempt::none) {
        return core::make_error(core::ErrorCategory::invalid_argument, "begin_connect", 0, "Use a fresh socket for each attempt");
    }
    auto configured = set_nonblocking(true);
    if (!configured) return configured.error();
    sockaddr_storage address{};
    int length = 0;
    core::Error conversion;
    if (!endpoint_to_sockaddr(remote, address, length, conversion)) return conversion;
    if (::connect(impl_->handle_, reinterpret_cast<const sockaddr*>(&address), length) == 0) {
        impl_->attempt_ = Impl::Attempt::connected;
        return ConnectState::connected;
    }
    const int code = ::WSAGetLastError();
    if (code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEALREADY || code == WSAEINTR) {
        impl_->attempt_ = Impl::Attempt::pending;
        return ConnectState::in_progress;
    }
    impl_->attempt_ = Impl::Attempt::failed;
    impl_->connect_error_ = code;
    return make_wsa_error("connect", code);
}

core::Result<void> Socket::finish_connect() {
    if (!is_valid()) return make_wsa_error("finish_connect", WSAENOTSOCK);
    if (impl_->attempt_ == Impl::Attempt::failed) return make_wsa_error("finish_connect", impl_->connect_error_);
    int pending_error = 0;
    int length = sizeof(pending_error);
    if (::getsockopt(impl_->handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending_error), &length) == SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        return make_wsa_error("getsockopt(SO_ERROR)", code);
    }
    if (pending_error != 0) {
        impl_->attempt_ = Impl::Attempt::failed;
        impl_->connect_error_ = pending_error;
        return make_wsa_error("finish_connect", pending_error);
    }
    // SO_ERROR can also be zero before an attempt has finished, or on a socket
    // that has never connected. Verify the peer without consuming application data.
    auto peer = remote_endpoint();
    if (!peer) {
        if (peer.error().category == core::ErrorCategory::not_connected && impl_->attempt_ == Impl::Attempt::pending) {
            return core::make_error(core::ErrorCategory::would_block, "finish_connect");
        }
        return peer.error();
    }
    impl_->attempt_ = Impl::Attempt::connected;
    return {};
}

core::Result<void> Socket::set_nonblocking(bool enabled) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "set_nonblocking", 0, "Socket is not valid");
    }

    u_long mode = enabled ? 1 : 0;
    if (::ioctlsocket(impl_->handle_, static_cast<long>(FIONBIO), &mode) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("ioctlsocket(FIONBIO)", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::set_reuse_address(bool enabled) {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "set_reuse_address", 0, "Socket is not valid");
    }

    int val = enabled ? 1 : 0;
    if (::setsockopt(
            impl_->handle_,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&val),
            sizeof(val)
        ) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("setsockopt(SO_REUSEADDR)", err);
    }
    return core::Result<void>::success();
}

core::Result<void> Socket::shutdown_write() {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "shutdown_write", 0, "Socket is not valid");
    }

    if (::shutdown(impl_->handle_, SD_SEND) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("shutdown(SD_SEND)", err);
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

    int to_read = static_cast<int>(std::min<std::size_t>(destination.size(), kMaxChunkSize));

    while (true) {
        int res = ::recv(
            impl_->handle_,
            reinterpret_cast<char*>(destination.data()),
            to_read,
            0
        );

        if (res == SOCKET_ERROR) {
            int err = ::WSAGetLastError();
            if (err == WSAEINTR) {
                continue; // Retry interrupted native operation
            }
            if (err == WSAEWOULDBLOCK) {
                return ReadResult::would_block_result();
            }
            return ReadResult::failure(make_wsa_error("recv", err));
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

    int to_write = static_cast<int>(std::min<std::size_t>(source.size(), kMaxChunkSize));

    while (true) {
        int res = ::send(
            impl_->handle_,
            reinterpret_cast<const char*>(source.data()),
            to_write,
            0
        );

        if (res == SOCKET_ERROR) {
            int err = ::WSAGetLastError();
            if (err == WSAEINTR) {
                continue; // Retry interrupted native operation
            }
            if (err == WSAEWOULDBLOCK) {
                return WriteResult::would_block_result();
            }
            return WriteResult::failure(make_wsa_error("send", err));
        }

        return WriteResult::success(static_cast<std::size_t>(res));
    }
}

core::Result<Endpoint> Socket::local_endpoint() const {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "local_endpoint", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    int len = sizeof(addr);
    if (::getsockname(impl_->handle_, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("getsockname", err);
    }

    return sockaddr_to_endpoint(addr, len);
}

core::Result<Endpoint> Socket::remote_endpoint() const {
    if (!is_valid()) {
        return core::make_error(core::ErrorCategory::bad_descriptor, "remote_endpoint", 0, "Socket is not valid");
    }

    sockaddr_storage addr{};
    int len = sizeof(addr);
    if (::getpeername(impl_->handle_, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        int err = ::WSAGetLastError();
        return make_wsa_error("getpeername", err);
    }

    return sockaddr_to_endpoint(addr, len);
}

// Free functions
core::Result<Socket> create_tcp_socket(AddressFamily family) {
    if (family != AddressFamily::ipv4 && family != AddressFamily::ipv6) {
        return core::make_error(core::ErrorCategory::invalid_argument, "socket", 0, "Unsupported address family");
    }
    int af = (family == AddressFamily::ipv6) ? AF_INET6 : AF_INET;
    SOCKET h = ::socket(af, SOCK_STREAM, IPPROTO_TCP);
    if (h == INVALID_SOCKET) {
        int err = ::WSAGetLastError();
        return make_wsa_error("socket", err);
    }
    try {
        return Socket(std::make_unique<Socket::Impl>(h));
    } catch (...) {
        ::closesocket(h);
        throw;
    }
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

core::Result<ConnectState> begin_connect(Socket& socket, const Endpoint& remote) { return socket.begin_connect(remote); }
core::Result<void> finish_connect(Socket& socket) { return socket.finish_connect(); }

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
