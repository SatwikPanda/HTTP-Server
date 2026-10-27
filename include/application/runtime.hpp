#pragma once
#include "core/config.hpp"
#include "http/application.hpp"
#include "net/endpoint.hpp"
#include <memory>
#include <string>

namespace application {
// NetworkRuntime must outlive this owner. Destruction joins drivers before helpers.
class Runtime {
public:
    // A custom factory replaces the demo application; static/proxy modes reject
    // overrides so their configuration cannot be silently ignored.
    [[nodiscard]] static core::Result<std::unique_ptr<Runtime>> start(core::RuntimeConfig config,
        std::shared_ptr<const http::TransactionFactory> custom_application = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void request_stop() noexcept;
    void join() noexcept;
    [[nodiscard]] net::Endpoint local_endpoint() const;
    // Bounded lifetime totals and current gauges; format off the event loop.
    [[nodiscard]] std::string statistics_json() const;
private:
    struct State;
    explicit Runtime(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace application
