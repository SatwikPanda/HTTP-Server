#include "application/server.hpp"

#include "application/runtime.hpp"
#include "net/runtime.hpp"

#include <optional>
#include <utility>

namespace application {
struct Server::Impl {
    ServerOptions options;
    http::Router router;
    // Destruction order: runtime joins before the networking lifetime ends.
    std::optional<net::NetworkRuntime> network;
    std::unique_ptr<Runtime> runtime;
};

Server::Server(ServerOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
}
Server::~Server() = default;

core::Result<void> Server::get(std::string path, http::RouteHandler handler) {
    if (impl_->runtime) {
        return core::make_error(core::ErrorCategory::invalid_argument, "register GET route", 0,
            "Register routes before listening");
    }
    return impl_->router.get(std::move(path), std::move(handler));
}

core::Result<net::Endpoint> Server::listen(std::uint32_t port, std::string bind_address) {
    if (impl_->runtime) return core::make_error(core::ErrorCategory::already_connected, "listen");
    const auto& options = impl_->options;
    if (port > 65535 || (options.driver != core::DriverMode::single &&
        options.driver != core::DriverMode::thread_pool && options.driver != core::DriverMode::event_loop)) {
        return core::make_error(core::ErrorCategory::invalid_argument, "listen configuration");
    }
    core::RuntimeConfig config;
    config.port = static_cast<std::uint16_t>(port);
    config.bind_address = std::move(bind_address);
    config.driver = options.driver;
    config.workers = options.workers;
    config.sessions = options.max_sessions;
    config.accept_queue = options.accept_queue;
    config.max_requests = options.max_requests_per_connection;
    config.buffers = options.buffers;
    config.idle_timeout = options.idle_timeout;
    config.progress_timeout = options.progress_timeout;
    config.drain_timeout = options.drain_timeout;
    // The runtime reserves one completion slot per pool worker.
    config.completion_queue = options.workers;
    auto valid = core::validate_config(config);
    if (!valid) return valid.error();
    auto network = net::initialize_network();
    if (!network) return network.error();
    auto started = Runtime::start(std::move(config), impl_->router.factory());
    if (!started) return started.error();
    impl_->network.emplace(std::move(network.value()));
    impl_->runtime = std::move(started.value());
    return impl_->runtime->local_endpoint();
}

void Server::request_stop() noexcept {
    if (impl_->runtime) impl_->runtime->request_stop();
}
void Server::wait() noexcept {
    if (impl_->runtime) impl_->runtime->join();
}
} // namespace application
