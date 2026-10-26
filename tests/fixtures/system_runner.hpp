#pragma once

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
    if (spec.application == core::ApplicationMode::proxy) {
        return spec.cache_enabled ? FeatureAvailability::pending_track_k_cached_proxy
                                  : FeatureAvailability::pending_track_b_proxy;
    }
    if (spec.driver == core::DriverMode::thread_pool) {
        return FeatureAvailability::pending_track_f_thread_pool;
    }
    if (spec.driver == core::DriverMode::event_loop) {
        return FeatureAvailability::pending_track_g_event_loop;
    }
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
        : spec_(spec),
          listener_(listener()),
          endpoint_(take(listener_.local_endpoint())) {
        diagnostics_.scenario_name = spec_.name;
        diagnostics_.application = spec_.application;
        diagnostics_.driver = spec_.driver;
        diagnostics_.cache_enabled = spec_.cache_enabled;

        if (spec_.application == core::ApplicationMode::static_files) {
            server::StaticConfig scfg;
            scfg.document_root = spec_.document_root;
            factory_ = take(server::make_static_factory(scfg));
        } else if (spec_.application == core::ApplicationMode::demo) {
            http::Response demo_resp(http::StatusCode::OK);
            demo_resp.set_body("Hello, World!\r\n", "text/plain; charset=utf-8");
            factory_ = http::make_demo_factory(std::move(demo_resp));
        } else {
            CHECK(false); // Caller must check_scenario_availability() first.
        }

        server_thread_ = std::thread([this] { accept_loop(); });
    }

    ~LoopbackSystemHarness() {
        stop();
        join();
    }

    LoopbackSystemHarness(const LoopbackSystemHarness&) = delete;
    LoopbackSystemHarness& operator=(const LoopbackSystemHarness&) = delete;

    [[nodiscard]] const net::Endpoint& endpoint() const noexcept { return endpoint_; }

    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
    }

    void join() noexcept {
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    [[nodiscard]] ScenarioDiagnostics diagnostics() const {
        std::lock_guard lock(diag_mutex_);
        return diagnostics_;
    }

    // Connect a client socket, send raw request wire bytes, and read until the expected
    // number of HTTP responses have been received or peer closes.
    [[nodiscard]] std::string send_and_receive(std::string_view wire_request) {
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
    void set_transition(std::string_view state) {
        std::lock_guard lock(diag_mutex_);
        diagnostics_.last_transition = std::string(state);
    }

    void accept_loop() {
        auto poller = take(net::Poller::create());
        auto token = take(poller.watch(listener_, net::Interest::read, {500, 1}));
        const auto deadline = Clock::now() + 10s;
        set_transition("listening");

        std::uint64_t conn_seq = 0;
        while (!stop_.load(std::memory_order_acquire) && Clock::now() < deadline) {
            auto events = take(poller.wait(std::min(deadline, Clock::now() + 20ms)));
            bool ready = false;
            for (const auto& ev : events) {
                if (ev.token == token && poller.is_current(ev)) ready = true;
            }
            if (!ready) continue;

            auto accepted = listener_.accept();
            if (!accepted) {
                if (accepted.error().category == core::ErrorCategory::would_block) continue;
                break;
            }
            net::Socket client_sock = std::move(accepted.value());
            {
                std::lock_guard lock(diag_mutex_);
                ++diagnostics_.accepted_connections;
                diagnostics_.active_connections = 1;
                diagnostics_.last_transition = "serving_session";
            }

            http::HttpSessionConfig cfg;
            cfg.application = factory_;
            cfg.keep_alive = true;
            cfg.max_output_bytes = spec_.max_output_bytes;
            cfg.max_requests_per_connection = spec_.max_requests_per_connection;
            cfg.idle_timeout = 200ms;
            cfg.request_progress_timeout = 2000ms;
            cfg.output_progress_timeout = 2000ms;

            (void)conn_seq;
            (void)http::run_http_session(client_sock, cfg);
            client_sock.close();

            {
                std::lock_guard lock(diag_mutex_);
                diagnostics_.active_connections = 0;
                diagnostics_.last_transition = "session_closed";
            }
        }
        CHECK(poller.unwatch(token));
        listener_.close();
        set_transition("stopped");
    }

    SystemScenarioSpec spec_;
    net::Socket listener_;
    net::Endpoint endpoint_;
    std::shared_ptr<const http::TransactionFactory> factory_;
    std::atomic<bool> stop_{false};
    mutable std::mutex diag_mutex_;
    ScenarioDiagnostics diagnostics_;
    std::thread server_thread_;
};

} // namespace test
