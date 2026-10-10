#pragma once

#include "core/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <chrono>
#include <climits>
#include <cstdint>
#include <string>

namespace net::detail {
#ifdef _WIN32
using NativeSocket = SOCKET;
using PollFd = WSAPOLLFD;
inline int native_error() noexcept { return WSAGetLastError(); }
inline bool interrupted(int code) noexcept { return code == WSAEINTR; }
inline int poll_sockets(PollFd* fds, std::size_t count, int timeout) {
    return WSAPoll(fds, static_cast<ULONG>(count), timeout);
}
#else
using NativeSocket = int;
using PollFd = pollfd;
inline int native_error() noexcept { return errno; }
inline bool interrupted(int code) noexcept { return code == EINTR; }
inline int poll_sockets(PollFd* fds, std::size_t count, int timeout) {
    return ::poll(fds, static_cast<nfds_t>(count), timeout);
}
#endif
inline NativeSocket native_socket(std::uintptr_t handle) noexcept {
    return static_cast<NativeSocket>(handle);
}
inline int timeout_ms(std::chrono::steady_clock::time_point deadline) noexcept {
    auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return 0;
    auto remaining = deadline - now;
    if (remaining >= std::chrono::milliseconds(INT_MAX)) return INT_MAX;
    // Round up to avoid returning before a sub-millisecond deadline.
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
}
inline core::Error poll_error(int code) {
    return core::make_error(core::ErrorCategory::io_error, "poll", code,
                           "Native readiness wait failed");
}
} // namespace net::detail
