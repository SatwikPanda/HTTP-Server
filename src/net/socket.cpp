#include "net/platform.hpp"
#include "net/runtime.hpp"
#include <memory>

namespace net {

static std::unique_ptr<NetworkRuntime> g_runtime;

void Platform::initialize(std::error_code& ec) noexcept {
    ec.clear();
    if (!g_runtime) {
        auto res = initialize_network();
        if (!res) {
            ec = std::make_error_code(std::errc::network_down);
            return;
        }
        g_runtime = std::make_unique<NetworkRuntime>(std::move(res.value()));
    }
}

void Platform::shutdown() noexcept {
    g_runtime.reset();
}

} // namespace net