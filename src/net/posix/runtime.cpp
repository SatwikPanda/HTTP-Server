#ifndef _WIN32

#include "net/runtime.hpp"

namespace net {

NetworkRuntime::NetworkRuntime(NetworkRuntime&& other) noexcept
    : active_(other.active_) {
    other.active_ = false;
}

NetworkRuntime& NetworkRuntime::operator=(NetworkRuntime&& other) noexcept {
    if (this != &other) {
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

NetworkRuntime::~NetworkRuntime() {
    active_ = false;
}

core::Result<NetworkRuntime> initialize_network() {
    return NetworkRuntime(true);
}

} // namespace net

#endif
