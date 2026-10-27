#pragma once

#include "application/server.hpp"

namespace app {
[[nodiscard]] core::Result<void> register_routes(application::Server& server);
}
