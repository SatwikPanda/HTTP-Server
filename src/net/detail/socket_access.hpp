#pragma once

#include "net/socket.hpp"
#include <cstdint>
#include <memory>

namespace net::detail {
struct SocketLifetime {};
// Internal bridge only; native descriptors never appear in public headers.
struct SocketAccess {
    static std::uintptr_t handle(const Socket&) noexcept;
    static std::weak_ptr<SocketLifetime> lifetime(const Socket&) noexcept;
};
} // namespace net::detail
