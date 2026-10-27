#pragma once

#include "http/application.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace http {

using RouteHandler = std::function<core::Result<Response>(const Request&)>;

// Configure on one thread, then publish an immutable factory snapshot. Handlers
// may run concurrently; captured mutable state must provide its own synchronization.
class Router {
public:
    static constexpr std::size_t max_routes = 1024;
    // Exact, case-sensitive escaped paths, excluding the query string. GET also
    // handles HEAD; the session suppresses its body while preserving its length.
    [[nodiscard]] core::Result<void> get(std::string path, RouteHandler handler);
    [[nodiscard]] std::shared_ptr<const TransactionFactory> factory() const;
private:
    std::unordered_map<std::string, std::shared_ptr<const RouteHandler>> routes_;
};

} // namespace http
