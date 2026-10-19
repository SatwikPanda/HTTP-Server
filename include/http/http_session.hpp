#pragma once

#include "concurrency/session.hpp"
#include "core/clock.hpp"
#include "core/result.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "net/socket.hpp"
#include "net/stream.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace http {

struct HttpSessionConfig {
    // Whole request head: request line, field lines, and the blank line.
    std::size_t max_header_bytes{8192};
    std::size_t max_request_line_bytes{8192};
    std::size_t max_target_bytes{8000};
    std::size_t max_header_count{100};
    // Request bytes held per connection: parsed head/body/extension/trailer bytes
    // plus unconsumed input. A request that cannot fit is rejected with 431.
    std::size_t max_input_bytes{16 * 1024};
    std::size_t max_output_bytes{1024 * 1024};
    std::chrono::milliseconds wait_retry_delay{1};
    std::chrono::milliseconds retry_timeout{5000};
    std::chrono::milliseconds idle_timeout{0};
    std::chrono::milliseconds request_progress_timeout{0};
    std::chrono::milliseconds output_progress_timeout{0};
    std::size_t max_requests_per_connection{100};
    bool keep_alive{false};
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
    ParseError parse_error{ParseError::none};
    int response_status{}; // 0 until a response is prepared
    bool request_received{};
    bool response_sent{};
    bool header_limit_exceeded{};
    bool input_limit_exceeded{};
    bool completed_cleanly{};
    std::size_t requests_served{};
};

[[nodiscard]] core::Result<void> validate_http_session_config(const HttpSessionConfig& config);
[[nodiscard]] RequestLimits request_limits(const HttpSessionConfig& config) noexcept;
[[nodiscard]] core::Result<Response> make_fixed_response(
    const HttpSessionConfig& config = {},
    const core::SystemClock& clock = core::real_system_clock()
);

// Parses requests and answers bodyless GET/HEAD with the fixed demo response.
// Supports keep-alive and ordered pipelined requests when keep_alive is configured.
// Rejects malformed framing, nonempty GET/HEAD, Expect, disabled methods, and
// Upgrade, then closes. Clocks must outlive the session.
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
    [[nodiscard]] std::size_t input_limit() const noexcept override { return config_.max_input_bytes; }
    [[nodiscard]] std::size_t output_limit() const noexcept override { return config_.max_output_bytes; }
    [[nodiscard]] std::size_t peak_input_bytes() const noexcept override { return stats_.peak_input_bytes; }
    [[nodiscard]] std::size_t peak_output_bytes() const noexcept override { return stats_.peak_output_bytes; }
    [[nodiscard]] core::ConnectionId connection_id() const noexcept override { return id_; }
    [[nodiscard]] std::chrono::milliseconds idle_timeout() const noexcept override;
    [[nodiscard]] core::SteadyTimePoint last_progress_time() const noexcept override { return last_progress_; }
    [[nodiscard]] const core::Error& error() const noexcept override { return stats_.error; }
    [[nodiscard]] const HttpSessionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const RequestParser& parser() const noexcept { return parser_; }

    // Bytes received after the parsed request, kept for the next request.
    [[nodiscard]] std::string_view unconsumed_input() const noexcept { return input_; }

private:
    [[nodiscard]] concurrency::Action next_action();
    [[nodiscard]] concurrency::Action close(core::Error error = {});
    [[nodiscard]] concurrency::Action prepare_response(ParseError rejected);
    [[nodiscard]] concurrency::Action process_parsed_input();
    [[nodiscard]] std::size_t read_capacity() const noexcept;
    [[nodiscard]] bool should_keep_alive() const noexcept;
    [[nodiscard]] std::chrono::milliseconds effective_idle_timeout() const noexcept;
    [[nodiscard]] std::chrono::milliseconds effective_request_timeout() const noexcept;
    [[nodiscard]] std::chrono::milliseconds effective_output_timeout() const noexcept;

    HttpSessionConfig config_;
    core::ConnectionId id_;
    const core::SteadyClock* steady_clock_;
    const core::SystemClock* wall_clock_;
    core::SteadyTimePoint last_progress_;
    core::SteadyTimePoint drain_deadline_{};
    concurrency::SessionState state_{concurrency::SessionState::initial};
    HttpSessionStats stats_;
    RequestParser parser_;
    std::string input_;
    std::string output_;
    std::size_t output_offset_{};
    std::size_t requests_served_{};
    bool writing_{};
    bool waiting_{};
    bool stopping_{};
    bool close_demanded_{};
    bool peer_eof_{};
};

[[nodiscard]] HttpSessionStats run_http_session(
    net::StreamChannel& channel, const HttpSessionConfig& config = {}
);
[[nodiscard]] HttpSessionStats run_http_session(
    net::Socket& socket, const HttpSessionConfig& config = {}
);

} // namespace http
