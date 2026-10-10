#pragma once

#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/io_types.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace net {
namespace detail { struct SocketAccess; }

enum class ConnectState { connected, in_progress };

class Socket {
public:
    class Impl;

    Socket() noexcept;
    explicit Socket(std::unique_ptr<Impl> impl) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    ~Socket();

    [[nodiscard]] bool is_valid() const noexcept;
    void close() noexcept;

    [[nodiscard]] core::Result<void> bind(const Endpoint& local);
    [[nodiscard]] core::Result<void> listen(int backlog = 128);
    [[nodiscard]] core::Result<Socket> accept();
    [[nodiscard]] core::Result<void> connect(const Endpoint& remote);
    [[nodiscard]] core::Result<ConnectState> begin_connect(const Endpoint& remote);
    [[nodiscard]] core::Result<void> finish_connect();
    [[nodiscard]] core::Result<void> set_nonblocking(bool enabled);
    [[nodiscard]] core::Result<void> set_reuse_address(bool enabled);
    [[nodiscard]] core::Result<void> shutdown_write();

    [[nodiscard]] ReadResult read_some(std::span<std::byte> destination);
    [[nodiscard]] WriteResult write_some(std::span<const std::byte> source);

    [[nodiscard]] core::Result<Endpoint> local_endpoint() const;
    [[nodiscard]] core::Result<Endpoint> remote_endpoint() const;

    [[nodiscard]] Impl* get_impl() noexcept { return impl_.get(); }
    [[nodiscard]] const Impl* get_impl() const noexcept { return impl_.get(); }

private:
    friend struct detail::SocketAccess;
    std::unique_ptr<Impl> impl_;
};

// Free function API matching Section 3.2
[[nodiscard]] core::Result<Socket> create_tcp_socket(AddressFamily family = AddressFamily::ipv4);
[[nodiscard]] core::Result<void> bind(Socket& socket, const Endpoint& local);
[[nodiscard]] core::Result<void> listen(Socket& socket, int backlog = 128);
[[nodiscard]] core::Result<Socket> accept(Socket& listener);
[[nodiscard]] core::Result<void> connect(Socket& socket, const Endpoint& remote);
[[nodiscard]] core::Result<ConnectState> begin_connect(Socket& socket, const Endpoint& remote);
[[nodiscard]] core::Result<void> finish_connect(Socket& socket);
[[nodiscard]] core::Result<void> set_nonblocking(Socket& socket, bool enabled);
[[nodiscard]] core::Result<void> set_reuse_address(Socket& socket, bool enabled);
[[nodiscard]] core::Result<void> shutdown_write(Socket& socket);
void close(Socket& socket) noexcept;
[[nodiscard]] ReadResult read_some(Socket& socket, std::span<std::byte> destination);
[[nodiscard]] WriteResult write_some(Socket& socket, std::span<const std::byte> source);

} // namespace net
