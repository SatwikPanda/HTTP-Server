#pragma once

#include "core/result.hpp"

namespace net {

class NetworkRuntime {
public:
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;

    NetworkRuntime(NetworkRuntime&& other) noexcept;
    NetworkRuntime& operator=(NetworkRuntime&& other) noexcept;

    ~NetworkRuntime();

    [[nodiscard]] bool is_initialized() const noexcept {
        return active_;
    }

private:
    friend core::Result<NetworkRuntime> initialize_network();
    explicit NetworkRuntime(bool active) noexcept : active_(active) {}

    bool active_{false};
};

core::Result<NetworkRuntime> initialize_network();

} // namespace net
