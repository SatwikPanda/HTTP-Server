#pragma once

#include "concurrency/session.hpp"
#include "core/clock.hpp"
#include "core/result.hpp"
#include "http/response.hpp"
#include "net/socket.hpp"
#include "net/stream.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace http {

struct HttpSessionConfig {
    std::size_t max_header_bytes{8192};
    std::size_t max_output_bytes{1024 * 1024};
    std::chrono::milliseconds wait_retry_delay{1};
    std::chrono::milliseconds retry_timeout{5000};
    std::string response_body{"Hello, World!\r\n"};
    std::string content_type{"text/plain; charset=utf-8"};
    StatusCode response_status{StatusCode::OK};
    std::optional<core::SystemTimePoint> fixed_wall_time;
};

struct HttpSessionStats {
    std::size_t total_bytes_read{};
    std::size_t total_bytes_written{};
    std::size_t short_write_count{};
    std::size_t would_block_read_count{};
    std::size_t would_block_write_count{};
    std::size_t readiness_wait_count{};
    std::size_t peak_input_bytes{};
    std::size_t peak_output_bytes{};
    core::Error error{};
    bool request_received{};
    bool response_sent{};
    bool header_limit_exceeded{};
    bool completed_cleanly{};
};

[[nodiscard]] core::Result<void> validate_http_session_config(const HttpSessionConfig& config);
[[nodiscard]] core::Result<Response> make_fixed_response(
    const HttpSessionConfig& config = {},
    const core::SystemClock& clock = core::real_system_clock()
);

// Temporary bounded CRLF header detection, replaced by the milestone 06 parser.
// Serves one response then closes. Clocks are borrowed and must outlive the session.
class HttpSession final : public concurrency::ConnectionSession {
public:
    explicit HttpSession(
        HttpSessionConfig config = {},
        core::ConnectionId id = {1, 1},
        const core::SteadyClock& steady_clock = core::real_steady_clock(),
        const core::SystemClock& wall_clock = core::real_system_clock()
    );

    [[nodiscard]] concurrency::Action initial_action() override;
    [[nodiscard]] concurrency::Action on_event(const concurrency::Event& event) override;
    [[nodiscard]] concurrency::SessionState state() const noexcept override { return state_; }
    [[nodiscard]] bool is_closed() const noexcept override { return state_ == concurrency::SessionState::closed; }
    [[nodiscard]] std::size_t input_limit() const noexcept override { return config_.max_header_bytes; }
    [[nodiscard]] std::size_t output_limit() const noexcept override { return config_.max_output_bytes; }
    [[nodiscard]] std::size_t peak_input_bytes() const noexcept override { return stats_.peak_input_bytes; }
    [[nodiscard]] std::size_t peak_output_bytes() const noexcept override { return stats_.peak_output_bytes; }
    [[nodiscard]] core::ConnectionId connection_id() const noexcept override { return id_; }
    [[nodiscard]] std::chrono::milliseconds idle_timeout() const noexcept override { return config_.retry_timeout; }
    [[nodiscard]] core::SteadyTimePoint last_progress_time() const noexcept override { return last_progress_; }
    [[nodiscard]] const core::Error& error() const noexcept override { return stats_.error; }
    [[nodiscard]] const HttpSessionStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] concurrency::Action next_action();
    [[nodiscard]] concurrency::Action close(core::Error error = {});
    [[nodiscard]] concurrency::Action prepare_response(bool oversized);

    HttpSessionConfig config_;
    core::ConnectionId id_;
    const core::SteadyClock* steady_clock_;
    const core::SystemClock* wall_clock_;
    core::SteadyTimePoint last_progress_;
    core::SteadyTimePoint drain_deadline_{};
    concurrency::SessionState state_{concurrency::SessionState::initial};
    HttpSessionStats stats_;
    std::string input_;
    std::string output_;
    std::size_t output_offset_{};
    bool writing_{};
    bool waiting_{};
    bool stopping_{};
};

[[nodiscard]] HttpSessionStats run_http_session(
    net::StreamChannel& channel, const HttpSessionConfig& config = {}
);
[[nodiscard]] HttpSessionStats run_http_session(
    net::Socket& socket, const HttpSessionConfig& config = {}
);

} // namespace http
