#pragma once

#include "application/runtime.hpp"
#include "concurrency/lifecycle.hpp"
#include "concurrency/single_session_driver.hpp"
#include "core/config.hpp"
#include "http/application.hpp"
#include "http/http_session.hpp"
#include "http/reference_application.hpp"
#include "net/poller.hpp"
#include "net/socket.hpp"
#include "server/static_service.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace test {

struct ScenarioDiagnostics {
    std::string scenario_name{};
    core::ApplicationMode application{core::ApplicationMode::demo};
    core::DriverMode driver{core::DriverMode::single};
    bool cache_enabled{false};
    std::size_t accepted_connections{0};
    std::size_t active_connections{0};
    std::size_t queued_jobs{0};
    std::string last_transition{"created"};
};

enum class FeatureAvailability {
    available,
    pending_track_b_proxy,
    pending_track_k_cached_proxy,
    pending_track_f_thread_pool,
    pending_track_g_event_loop
};

struct SystemScenarioSpec {
    std::string name{};
    core::ApplicationMode application{core::ApplicationMode::static_files};
    core::DriverMode driver{core::DriverMode::single};
    bool cache_enabled{false};
    std::filesystem::path document_root{};
    std::size_t max_output_bytes{65536};
    std::size_t max_requests_per_connection{100};
};

// Checks whether the production modules required for a given scenario combination
// are compiled into this checkout. Never substitutes a mock for an absent module.
[[nodiscard]] inline FeatureAvailability check_scenario_availability(const SystemScenarioSpec& spec) noexcept {
    (void)spec; // All required production targets are linked by CMake.
    return FeatureAvailability::available;
}

[[nodiscard]] inline std::string_view to_string(FeatureAvailability avail) noexcept {
    switch (avail) {
        case FeatureAvailability::available: return "available";
        case FeatureAvailability::pending_track_b_proxy: return "pending (track B uncached proxy not yet merged)";
        case FeatureAvailability::pending_track_k_cached_proxy: return "pending (track K cached proxy not yet merged)";
        case FeatureAvailability::pending_track_f_thread_pool: return "pending (track F thread-pool driver not yet merged)";
        case FeatureAvailability::pending_track_g_event_loop: return "pending (track G event-loop driver not yet merged)";
    }
    return "pending";
}

// Loopback harness that runs a real production TransactionFactory + HttpSession
// over ephemeral loopback TCP sockets, recording bounded diagnostic progress.
class LoopbackSystemHarness {
public:
    explicit LoopbackSystemHarness(const SystemScenarioSpec& spec)
        : spec_(spec) {
        core::RuntimeConfig config;
        config.port = 0;
        config.application = spec.application;
        config.driver = spec.driver;
        config.cache.enabled = spec.cache_enabled;
        if (!spec.document_root.empty()) config.document_root = spec.document_root;
        config.buffers.output = spec.max_output_bytes;
        config.buffers.high_water = spec.max_output_bytes * 3 / 4;
        config.buffers.low_water = spec.max_output_bytes / 4;
        config.max_requests = spec.max_requests_per_connection;
        config.workers = 4;
        config.helpers = 2;
        config.idle_timeout = 500ms;
        config.progress_timeout = 2000ms;
        config.connect_timeout = 2000ms;
        config.work_timeout = 2000ms;
        config.drain_timeout = 100ms;
        app_ = take(application::Runtime::start(config));
        endpoint_ = app_->local_endpoint();
        diagnostics_.scenario_name = spec.name;
        diagnostics_.application = spec.application;
        diagnostics_.driver = spec.driver;
        diagnostics_.cache_enabled = spec.cache_enabled;
        diagnostics_.last_transition = "listening";
    }

    ~LoopbackSystemHarness() {
        stop();
        join();
    }

    LoopbackSystemHarness(const LoopbackSystemHarness&) = delete;
    LoopbackSystemHarness& operator=(const LoopbackSystemHarness&) = delete;

    [[nodiscard]] const net::Endpoint& endpoint() const noexcept { return endpoint_; }

    void stop() noexcept { app_->request_stop(); }
    void join() noexcept {
        app_->join();
        std::lock_guard lock(diag_mutex_);
        diagnostics_.last_transition = "stopped";
    }
    [[nodiscard]] std::string statistics_json() const { return app_->statistics_json(); }

    [[nodiscard]] ScenarioDiagnostics diagnostics() const {
        std::lock_guard lock(diag_mutex_);
        return diagnostics_;
    }

    // Connect a client socket, send raw request wire bytes, and read until the expected
    // number of HTTP responses have been received or peer closes.
    [[nodiscard]] std::string send_and_receive(std::string_view wire_request) {
        {
            std::lock_guard lock(diag_mutex_);
            ++diagnostics_.accepted_connections; // Client connections observed by this fixture.
        }
        auto client = take(net::create_tcp_socket());
        CHECK(client.connect(endpoint_));
        CHECK(client.set_nonblocking(true));

        auto poller = take(net::Poller::create());
        auto token = take(poller.watch(client, net::Interest::write, {900, 1}));
        const auto deadline = Clock::now() + 5s;

        auto wait_ready = [&](net::Interest interest) {
            CHECK(poller.modify(token, interest));
            while (Clock::now() < deadline) {
                auto events = take(poller.wait(std::min(deadline, Clock::now() + 25ms)));
                for (const auto& ev : events) {
                    if (ev.token == token && poller.is_current(ev)) return;
                }
            }
            CHECK(false);
        };

        std::size_t sent = 0;
        const auto raw_bytes = std::as_bytes(std::span(wire_request.data(), wire_request.size()));
        while (sent < raw_bytes.size()) {
            const auto wrote = client.write_some(raw_bytes.subspan(sent));
            if (wrote.is_ok()) {
                CHECK(wrote.bytes_transferred > 0);
                sent += wrote.bytes_transferred;
            } else {
                CHECK(wrote.would_block());
                wait_ready(net::Interest::write);
            }
        }

        std::string response;
        std::array<std::byte, 4096> buf{};
        while (Clock::now() < deadline) {
            const auto read = client.read_some(buf);
            if (read.is_eof()) break;
            if (read.is_ok()) {
                CHECK(response.size() + read.bytes_transferred <= 32 * 1024 * 1024);
                response.append(reinterpret_cast<const char*>(buf.data()), read.bytes_transferred);
            } else {
                CHECK(read.would_block());
                wait_ready(net::Interest::read);
            }
        }
        CHECK(poller.unwatch(token));
        client.close();
        return response;
    }

private:
    SystemScenarioSpec spec_;
    net::Endpoint endpoint_;
    std::unique_ptr<application::Runtime> app_;
    mutable std::mutex diag_mutex_;
    ScenarioDiagnostics diagnostics_;
};
} // namespace test
