#include "http/http_session.hpp"

#include "concurrency/single_session_driver.hpp"
#include "http/date.hpp"
#include "http/header.hpp"
#include "http/serializer.hpp"
#include "net/poller.hpp"

#include <algorithm>
#include <span>
#include <type_traits>
#include <utility>

namespace http {
namespace {

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

core::Result<void> validate_http_session_config(const HttpSessionConfig& config) {
    constexpr std::size_t hard_limit = 16 * 1024 * 1024;
    const int status = static_cast<int>(config.response_status);
    if (config.max_header_bytes < 4 || config.max_header_bytes > hard_limit ||
        config.max_output_bytes == 0 || config.max_output_bytes > hard_limit ||
        config.response_body.size() > config.max_output_bytes ||
        config.wait_retry_delay <= std::chrono::milliseconds::zero() ||
        config.wait_retry_delay > std::chrono::hours(24) ||
        config.retry_timeout <= std::chrono::milliseconds::zero() ||
        config.retry_timeout > std::chrono::hours(24) ||
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
    response.headers.set("Connection", "close");
    response.headers.set("Server", "httpserver/1.0");
    return response;
}

HttpSession::HttpSession(HttpSessionConfig config, core::ConnectionId id,
                         const core::SteadyClock& steady_clock, const core::SystemClock& wall_clock)
    : config_(std::move(config)), id_(id), steady_clock_(&steady_clock),
      wall_clock_(&wall_clock), last_progress_(steady_clock.now()) {
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

concurrency::Action HttpSession::next_action() {
    if (is_closed()) return concurrency::CloseAction{stats_.error};
    auto deadline = deadline_after(last_progress_, config_.retry_timeout);
    if (stopping_) deadline = std::min(deadline, drain_deadline_);
    if (steady_clock_->now() >= deadline) {
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
    return concurrency::ReadAction{std::min<std::size_t>(512, config_.max_header_bytes - input_.size())};
}

concurrency::Action HttpSession::initial_action() {
    return next_action();
}

concurrency::Action HttpSession::prepare_response(bool oversized) {
    auto response_config = config_;
    if (oversized) {
        stats_.header_limit_exceeded = true;
        response_config.response_status = StatusCode::HeaderFieldsTooLarge;
        response_config.response_body = "431 Request Header Fields Too Large\r\n";
        response_config.content_type = "text/plain; charset=utf-8";
    }
    auto response = make_fixed_response(response_config, *wall_clock_);
    if (!response) return close(response.error());
    auto wire = serialize_response(response.value(),
        {std::min<std::size_t>(8192, config_.max_output_bytes), config_.max_output_bytes});
    if (!wire) return close(wire.error());
    if (wire.value().size() > config_.max_output_bytes) return close(invalid_config());
    output_ = std::move(wire.value());
    stats_.peak_output_bytes = output_.size();
    input_.clear();
    output_offset_ = 0;
    writing_ = true;
    waiting_ = false;
    last_progress_ = steady_clock_->now();
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
            if (writing_ || value.data.empty() ||
                value.data.size() > std::min<std::size_t>(512, config_.max_header_bytes - input_.size())) {
                return close(core::make_error(core::ErrorCategory::io_error, "HTTP read", 0, "Invalid read event"));
            }
            waiting_ = false;
            last_progress_ = steady_clock_->now();
            for (std::byte byte : value.data) {
                input_.push_back(static_cast<char>(byte));
                stats_.peak_input_bytes = std::max(stats_.peak_input_bytes, input_.size());
                if (input_.ends_with("\r\n\r\n")) {
                    stats_.request_received = true;
                    return prepare_response(false);
                }
                if (input_.size() == config_.max_header_bytes) return prepare_response(true);
            }
            return next_action();
        } else if constexpr (std::is_same_v<T, ReadEofEvent>) {
            if (writing_) return next_action();
            if (input_.empty()) return close();
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
                return close();
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
            return next_action();
        } else if constexpr (std::is_same_v<T, TimeoutEvent>) {
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
