#ifdef _WIN32

#include "net/runtime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

namespace net {

NetworkRuntime::NetworkRuntime(NetworkRuntime&& other) noexcept
    : active_(other.active_) {
    other.active_ = false;
}

NetworkRuntime& NetworkRuntime::operator=(NetworkRuntime&& other) noexcept {
    if (this != &other) {
        if (active_) {
            ::WSACleanup();
        }
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

NetworkRuntime::~NetworkRuntime() {
    if (active_) {
        ::WSACleanup();
        active_ = false;
    }
}

core::Result<NetworkRuntime> initialize_network() {
    WSADATA wsa_data{};
    int result = ::WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return core::make_error(
            core::ErrorCategory::system_error,
            "initialize_network",
            result,
            "WSAStartup failed"
        );
    }
    return NetworkRuntime(true);
}

} // namespace net

#endif
