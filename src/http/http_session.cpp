#include "http/http_session.hpp"

#include "concurrency/single_session_driver.hpp"
#include "http/date.hpp"
#include "http/framing.hpp"
#include "http/header.hpp"
#include "http/serializer.hpp"
#include "net/poller.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace http {
namespace {

std::string_view trim_ws(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

core::Error invalid_config() {
    return core::make_error(core::ErrorCategory::invalid_argument, "HTTP session configuration");
}

core::SteadyTimePoint deadline_after(core::SteadyTimePoint now, std::chrono::milliseconds duration) {
    const auto ticks = std::chrono::duration_cast<core::SteadyTimePoint::duration>(duration);
    return now > core::SteadyTimePoint::max() - ticks ? core::SteadyTimePoint::max() : now + ticks;
}

HttpSessionStats session_stats(const HttpSession& session, const concurrency::DriverStats& driver) {
    auto stats = session.stats();
    stats.total_bytes_read = driver.total_bytes_read;
    stats.total_bytes_written = driver.total_bytes_written;
    stats.short_write_count = driver.short_write_count;
    stats.would_block_read_count = driver.would_block_read_count;
    stats.would_block_write_count = driver.would_block_write_count;
    stats.readiness_wait_count = driver.readiness_wait_count;
    stats.completed_cleanly = driver.completed_cleanly;
    if (driver.error) stats.error = driver.error;
    return stats;
}

} // namespace

RequestLimits request_limits(const HttpSessionConfig& config) noexcept {
    RequestLimits limits;
    limits.max_request_line_bytes = config.max_request_line_bytes;
    limits.max_target_bytes = config.max_target_bytes;
    limits.max_header_bytes = config.max_header_bytes;
    limits.max_header_count = config.max_header_count;
    return limits;
}

core::Result<void> validate_http_session_config(const HttpSessionConfig& config) {
    constexpr std::size_t hard_limit = 16 * 1024 * 1024;
    const int status = static_cast<int>(config.response_status);
    if (config.max_header_bytes < 4 || config.max_header_bytes > hard_limit ||
        config.max_input_bytes < 4 || config.max_input_bytes > hard_limit ||
        !validate_request_limits(request_limits(config)) ||
        config.max_output_bytes == 0 || config.max_output_bytes > hard_limit ||
        config.response_body.size() > config.max_output_bytes ||
        config.wait_retry_delay <= std::chrono::milliseconds::zero() ||
        config.wait_retry_delay > std::chrono::hours(24) ||
        config.retry_timeout <= std::chrono::milliseconds::zero() ||
        config.retry_timeout > std::chrono::hours(24) ||
        config.idle_timeout < std::chrono::milliseconds::zero() ||
        config.idle_timeout > std::chrono::hours(24) ||
        config.request_progress_timeout < std::chrono::milliseconds::zero() ||
        config.request_progress_timeout > std::chrono::hours(24) ||
        config.output_progress_timeout < std::chrono::milliseconds::zero() ||
        config.output_progress_timeout > std::chrono::hours(24) ||
        config.max_requests_per_connection == 0 ||
        !is_valid_header_value(config.content_type) || config.content_type.empty() ||
        status < 200 || !is_valid_status_code(status) || status == 204 || status == 304) {
        return invalid_config();
    }
    return {};
}

core::Result<Response> make_fixed_response(const HttpSessionConfig& config, const core::SystemClock& clock) {
    auto valid = validate_http_session_config(config);
    if (!valid) return valid.error();
    Response response(config.response_status);
    response.set_body(config.response_body, config.content_type);
    const auto date = format_http_date(config.fixed_wall_time.value_or(clock.now()));
    if (date.empty()) return core::make_error(core::ErrorCategory::invalid_argument, "HTTP date");
    response.headers.set("Date", date);
    response.headers.set("Connection", config.keep_alive ? "keep-alive" : "close");
    response.headers.set("Server", "httpserver/1.0");
    if (config.response_status == StatusCode::MethodNotAllowed) {
        response.headers.set("Allow", "GET, HEAD");
    }
    return response;
}

HttpSession::HttpSession(HttpSessionConfig config, core::ConnectionId id,
                         const core::SteadyClock& steady_clock, const core::SystemClock& wall_clock)
    : config_(std::move(config)), id_(id), steady_clock_(&steady_clock),
      wall_clock_(&wall_clock), last_progress_(steady_clock.now()),
      parser_(request_limits(config_)) {
    auto valid = validate_http_session_config(config_);
    if (!valid) {
        stats_.error = valid.error();
        state_ = concurrency::SessionState::closed;
    }
}

concurrency::Action HttpSession::close(core::Error error) {
    stats_.error = std::move(error);
    state_ = concurrency::SessionState::closed;
    return concurrency::CloseAction{stats_.error};
}

std::chrono::milliseconds HttpSession::effective_idle_timeout() const noexcept {
    return config_.idle_timeout > std::chrono::milliseconds::zero() ? config_.idle_timeout : config_.retry_timeout;
}

std::chrono::milliseconds HttpSession::effective_request_timeout() const noexcept {
    return config_.request_progress_timeout > std::chrono::milliseconds::zero() ? config_.request_progress_timeout : config_.retry_timeout;
}

std::chrono::milliseconds HttpSession::effective_output_timeout() const noexcept {
    return config_.output_progress_timeout > std::chrono::milliseconds::zero() ? config_.output_progress_timeout : config_.retry_timeout;
}

std::chrono::milliseconds HttpSession::idle_timeout() const noexcept {
    if (writing_) return effective_output_timeout();
    if (parser_.head_bytes() > 0 || !input_.empty()) return effective_request_timeout();
    return effective_idle_timeout();
}

bool HttpSession::should_keep_alive() const noexcept {
    if (!config_.keep_alive) return false;
    if (stopping_) return false;
    if (stats_.error) return false;
    if (close_demanded_) return false;
    if (requests_served_ >= config_.max_requests_per_connection) return false;
    if (peer_eof_) return false;
    return true;
}

concurrency::Action HttpSession::next_action() {
    if (is_closed()) return concurrency::CloseAction{stats_.error};
    auto deadline = deadline_after(last_progress_, idle_timeout());
    if (stopping_) deadline = std::min(deadline, drain_deadline_);
    if (steady_clock_->now() >= deadline) {
        if (requests_served_ > 0 && parser_.head_bytes() == 0 && input_.empty()) {
            return close();
        }
        if (writing_) {
            return close(core::make_error(core::ErrorCategory::timed_out, "HTTP output progress"));
        }
        if (parser_.head_bytes() > 0 || !input_.empty()) {
            return close(core::make_error(core::ErrorCategory::timed_out, "HTTP request progress"));
        }
        return close(core::make_error(core::ErrorCategory::timed_out, "HTTP progress"));
    }
    if (waiting_) {
        state_ = concurrency::SessionState::waiting;
        return concurrency::WaitAction{writing_ ? net::Interest::write : net::Interest::read, deadline};
    }
    if (writing_) {
        state_ = stopping_ ? concurrency::SessionState::draining : concurrency::SessionState::writing;
        return concurrency::WriteAction{std::as_bytes(std::span(output_.data() + output_offset_,
                                                               output_.size() - output_offset_))};
    }
    state_ = concurrency::SessionState::reading;
    return concurrency::ReadAction{read_capacity()};
}

// Include retained bodies, extensions, and trailers in the connection budget.
std::size_t HttpSession::read_capacity() const noexcept {
    const std::size_t held = parser_.buffered_bytes() + input_.size();
    const std::size_t input_room = config_.max_input_bytes - std::min(config_.max_input_bytes, held);
    if (!parser_.head_complete()) {
        const std::size_t header_room = config_.max_header_bytes - std::min(config_.max_header_bytes, parser_.head_bytes());
        return std::min({std::size_t{512}, header_room, input_room});
    }
    return std::min({std::size_t{512}, input_room});
}

concurrency::Action HttpSession::initial_action() {
    return next_action();
}

concurrency::Action HttpSession::prepare_response(ParseError rejected) {
    auto response_config = config_;
    bool will_keep_alive = false;
    if (rejected != ParseError::none) {
        stats_.parse_error = rejected;
        if ((rejected == ParseError::header_too_large || rejected == ParseError::too_many_headers) &&
            !stats_.input_limit_exceeded) {
            stats_.header_limit_exceeded = true;
        }
        const StatusCode status = status_for(rejected);
        response_config.response_status = status;
        response_config.response_body = std::to_string(static_cast<int>(status)) + " " +
                                         std::string(default_reason_phrase(status)) + "\r\n";
        response_config.content_type = "text/plain; charset=utf-8";
        close_demanded_ = true;
    } else if (config_.keep_alive && !stopping_ && !close_demanded_ && !peer_eof_) {
        const auto& req = parser_.request();
        bool client_wants_close = false;
        bool client_wants_keep_alive = false;
        for (const auto& field : req.headers) {
            if (!header_name_equals(field.name, "Connection")) continue;
            std::string_view rest = field.value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                auto token = trim_ws(rest.substr(0, comma));
                if (header_name_equals(token, "close")) client_wants_close = true;
                if (header_name_equals(token, "keep-alive")) client_wants_keep_alive = true;
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            }
        }
        if (req.version == HttpVersion::http_1_1()) {
            will_keep_alive = !client_wants_close;
        } else if (req.version == HttpVersion::http_1_0()) {
            will_keep_alive = client_wants_keep_alive && !client_wants_close;
        }
        if (requests_served_ + 1 >= config_.max_requests_per_connection) {
            will_keep_alive = false;
        }
        if (!will_keep_alive) {
            close_demanded_ = true;
        }
    } else {
        close_demanded_ = true;
    }

    response_config.keep_alive = will_keep_alive;
    auto response = make_fixed_response(response_config, *wall_clock_);
    if (!response) return close(response.error());

    if (will_keep_alive) {
        response.value().headers.set("Connection", "keep-alive");
    } else {
        response.value().headers.set("Connection", "close");
    }

    auto wire = serialize_response(response.value(), parser_.request().method,
        {std::min<std::size_t>(8192, config_.max_output_bytes), config_.max_output_bytes});
    if (!wire) return close(wire.error());
    if (wire.value().size() > config_.max_output_bytes) return close(invalid_config());
    output_ = std::move(wire.value());
    stats_.peak_output_bytes = std::max(stats_.peak_output_bytes, output_.size());
    stats_.response_status = response.value().status_code;
    output_offset_ = 0;
    writing_ = true;
    waiting_ = false;
    last_progress_ = steady_clock_->now();
    return next_action();
}

concurrency::Action HttpSession::process_parsed_input() {
    const auto parsed = parser_.consume(input_);
    input_.erase(0, parsed.consumed);
    if (parsed.event == ParseEvent::complete) stats_.request_received = true;
    if (parsed.event != ParseEvent::error && parser_.head_complete()) {
        const auto restriction = check_request_restrictions(parser_.request());
        if (restriction != StatusCode::OK) {
            close_demanded_ = true;
            if (restriction == StatusCode::MethodNotAllowed) {
                return prepare_response(ParseError::method_not_allowed);
            } else if (restriction == StatusCode::ExpectationFailed) {
                return prepare_response(ParseError::expectation_failed);
            } else if (restriction == StatusCode::NotImplemented) {
                return prepare_response(ParseError::not_implemented);
            } else {
                return prepare_response(ParseError::nonempty_get_head_body);
            }
        }
    }
    if (parsed.event == ParseEvent::complete) {
        return prepare_response(ParseError::none);
    }
    if (parsed.event == ParseEvent::error) {
        close_demanded_ = true;
        // The stream can no longer be framed, so answer and close.
        return prepare_response(parser_.error());
    }
    if (read_capacity() == 0) {
        // An incomplete request has filled a bound the parser cannot see past.
        if (parser_.head_bytes() < config_.max_header_bytes) stats_.input_limit_exceeded = true;
        close_demanded_ = true;
        return prepare_response(ParseError::header_too_large);
    }
    return next_action();
}

concurrency::Action HttpSession::on_event(const concurrency::Event& event) {
    using namespace concurrency;
    if (is_closed()) return CloseAction{stats_.error};
    return event.visit([&](const auto& value) -> Action {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ReadDataEvent>) {
            auto pending = next_action();
            if (pending.is_close()) return pending;
            if (writing_ || value.data.empty() || value.data.size() > read_capacity()) {
                return close(core::make_error(core::ErrorCategory::io_error, "HTTP read", 0, "Invalid read event"));
            }
            waiting_ = false;
            last_progress_ = steady_clock_->now();
            input_.append(reinterpret_cast<const char*>(value.data.data()), value.data.size());
            stats_.peak_input_bytes = std::max(stats_.peak_input_bytes, parser_.buffered_bytes() + input_.size());
            return process_parsed_input();
        } else if constexpr (std::is_same_v<T, ReadEofEvent>) {
            peer_eof_ = true;
            if (writing_) return next_action();
            if (parser_.head_bytes() == 0 && input_.empty()) return close();
            return close(core::make_error(core::ErrorCategory::eof, "HTTP header", 0, "Incomplete header block"));
        } else if constexpr (std::is_same_v<T, WriteCompleteEvent>) {
            auto pending = next_action();
            if (pending.is_close()) return pending;
            if (!writing_ || value.bytes_transferred == 0 ||
                value.bytes_transferred > output_.size() - output_offset_) {
                return close(core::make_error(core::ErrorCategory::io_error, "HTTP write", 0, "Invalid write event"));
            }
            output_offset_ += value.bytes_transferred;
            last_progress_ = steady_clock_->now();
            waiting_ = false;
            if (output_offset_ == output_.size()) {
                stats_.response_sent = true;
                ++stats_.requests_served;
                ++requests_served_;
                if (!should_keep_alive()) {
                    return close();
                }
                output_.clear();
                output_offset_ = 0;
                writing_ = false;
                waiting_ = false;
                parser_.reset();
                last_progress_ = steady_clock_->now();
                if (!input_.empty()) {
                    return process_parsed_input();
                }
                return next_action();
            }
            return next_action();
        } else if constexpr (std::is_same_v<T, WouldBlockEvent>) {
            const auto expected = writing_ ? net::Interest::write : net::Interest::read;
            if (value.interest != expected) {
                return close(core::make_error(core::ErrorCategory::io_error, "HTTP wait"));
            }
            waiting_ = true;
            return next_action();
        } else if constexpr (std::is_same_v<T, ReadyEvent>) {
            if (writing_ ? value.writable : value.readable) waiting_ = false;
            return next_action();
        } else if constexpr (std::is_same_v<T, StopEvent>) {
            if (!writing_ || value.drain_timeout <= std::chrono::milliseconds::zero()) return close();
            const auto deadline = deadline_after(steady_clock_->now(),
                std::min(value.drain_timeout, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24))));
            drain_deadline_ = stopping_ ? std::min(drain_deadline_, deadline) : deadline;
            stopping_ = true;
            close_demanded_ = true;
            return next_action();
        } else if constexpr (std::is_same_v<T, TimeoutEvent>) {
            if (requests_served_ > 0 && parser_.head_bytes() == 0 && input_.empty()) {
                return close();
            }
            if (writing_) {
                return close(core::make_error(core::ErrorCategory::timed_out, "HTTP output progress"));
            }
            if (parser_.head_bytes() > 0 || !input_.empty()) {
                return close(core::make_error(core::ErrorCategory::timed_out, "HTTP request progress"));
            }
            return close(core::make_error(core::ErrorCategory::timed_out, "HTTP progress"));
        } else if constexpr (std::is_same_v<T, ErrorEvent>) {
            return close(value.error);
        }
    });
}

HttpSessionStats run_http_session(net::StreamChannel& channel, const HttpSessionConfig& config) {
    HttpSession session(config);
    if (!channel.is_valid() && !session.is_closed()) {
        auto stats = session.stats();
        stats.error = core::make_error(core::ErrorCategory::bad_descriptor, "HTTP channel");
        return stats;
    }
    concurrency::SingleSessionDriver driver;
    const auto driven = driver.drive_channel(channel, session, config.wait_retry_delay);
    return session_stats(session, driven);
}

HttpSessionStats run_http_session(net::Socket& socket, const HttpSessionConfig& config) {
    HttpSession session(config);
    if (session.is_closed()) return session.stats();
    auto configured = socket.set_nonblocking(true);
    if (!configured) { auto stats = session.stats(); stats.error = configured.error(); return stats; }
    auto created = net::Poller::create();
    if (!created) { auto stats = session.stats(); stats.error = created.error(); return stats; }
    auto poller = std::move(created.value());
    auto watched = poller.watch(socket, net::Interest::read, session.connection_id());
    if (!watched) { auto stats = session.stats(); stats.error = watched.error(); return stats; }
    struct Guard {
        net::Poller& poller;
        net::RegistrationToken token;
        ~Guard() { (void)poller.unwatch(token); }
    } guard{poller, watched.value()};
    concurrency::SingleSessionDriver driver;
    return session_stats(session, driver.drive(socket, session, poller, watched.value()));
}

} // namespace http
