#include "routes.hpp"

namespace app {
core::Result<void> register_routes(application::Server& server) {
    return server.get("/", [](const http::Request&) {
        return http::Response::text("Hello, World!");
    });
}
} // namespace app
