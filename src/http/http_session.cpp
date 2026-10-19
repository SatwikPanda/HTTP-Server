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
        config.max_requests_per_connection == 0 || config.metrics == nullptr ||
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
      parser_(request_limits(config_)), writer_(config_.max_output_bytes) {
    auto valid = validate_http_session_config(config_);
    if (!valid) {
        stats_.error = valid.error();
        state_ = concurrency::SessionState::closed;
    }
}

concurrency::Action HttpSession::close(core::Error error) {
    if (error && !is_closed()) config_.metrics->record(core::Metric::request_errors);
    if (cancellation_) cancellation_->cancel();
    work_.reset();
    transaction_.reset();
    writer_.abort();
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
    if (writing_ || transaction_) return effective_output_timeout();
    if (parser_.head_bytes() > 0 || !input_.empty()) return effective_request_timeout();
    return effective_idle_timeout();
}

bool HttpSession::should_keep_alive() const noexcept {
    if (!config_.keep_alive) return false;
    if (stopping_) return false;
    if (stats_.error) return false;
    if (close_demanded_) return false;
    if (requests_served_ >= config_.max_requests_per_connection) return false;
    if (peer_eof_ && input_.empty()) return false;
    return true;
}

concurrency::Action HttpSession::next_action() {
    if (is_closed()) return concurrency::CloseAction{stats_.error};
    auto deadline = deadline_after(last_progress_, idle_timeout());
    if (stopping_) deadline = std::min(deadline, drain_deadline_);
    if (steady_clock_->now() >= deadline) {
        if (!writing_ && requests_served_ > 0 && parser_.head_bytes() == 0 && input_.empty()) {
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
    if (work_) {
        work_->deadline = std::min(work_->deadline, deadline);
        state_ = concurrency::SessionState::waiting;
        return *work_;
    }
    if (waiting_) {
        state_ = concurrency::SessionState::waiting;
        return concurrency::WaitAction{writing_ ? net::Interest::write : net::Interest::read, deadline, downstream_id()};
    }
    if (writing_) {
        state_ = stopping_ ? concurrency::SessionState::draining : concurrency::SessionState::writing;
        return concurrency::WriteAction{writer_.pending(), downstream_id()};
    }
    state_ = concurrency::SessionState::reading;
    return concurrency::ReadAction{read_capacity(), downstream_id()};
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
    config_.metrics->record(core::Metric::requests_started);
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
    } else if (config_.keep_alive && !stopping_ && !close_demanded_ &&
               (!peer_eof_ || !input_.empty())) {
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
    cancellation_ = std::make_shared<concurrency::Cancellation>();
    if (rejected != ParseError::none) {
        head_started_ = true;
        auto wire = writer_.buffered(response.value(), parser_.request().method);
        if (!wire) return close(wire.error());
        stats_.response_status = response.value().status_code;
        writing_ = true; waiting_ = false;
        stats_.peak_output_bytes = std::max(stats_.peak_output_bytes, writer_.pending().size());
        last_progress_ = steady_clock_->now();
        return next_action();
    }
    auto factory = config_.application ? config_.application : make_demo_factory(std::move(response.value()));
    auto created = factory->create(parser_.request(), {{id_, requests_served_ + 1}, config_.max_output_bytes, *steady_clock_, *wall_clock_});
    if (!created) return transaction_failure(created.error(), StatusCode::InternalServerError);
    if (!created.value()) return transaction_failure(invalid_config(), StatusCode::InternalServerError);
    transaction_ = std::move(created.value());
    return advance_transaction(StartTransaction{});
}

core::OperationId HttpSession::downstream_id() const noexcept {
    return {{id_, requests_served_ + 1}, core::EndpointRole::downstream, downstream_sequence_};
}

concurrency::Action HttpSession::transaction_failure(core::Error error, StatusCode status) {
    if (writer_.committed()) return close(std::move(error));
    config_.metrics->record(core::Metric::request_errors);
    if (cancellation_) cancellation_->cancel();
    work_.reset(); transaction_.reset();
    writer_ = ResponseWriter(config_.max_output_bytes);
    close_demanded_ = true;
    Response response(status);
    if (response.status_code < 400 || response.status_code > 599) response = Response(StatusCode::InternalServerError);
    response.set_body(std::to_string(response.status_code) + " Error\r\n", "text/plain; charset=utf-8");
    response.headers.set("Connection", "close");
    auto encoded = writer_.buffered(response, parser_.request().method);
    if (!encoded) return close(encoded.error());
    stats_.response_status = response.status_code;
    writing_ = true; waiting_ = false; head_started_ = true;
    stats_.peak_output_bytes = std::max(stats_.peak_output_bytes, writer_.pending().size());
    last_progress_ = steady_clock_->now();
    return next_action();
}

concurrency::Action HttpSession::advance_transaction(const TransactionEvent& event) {
    auto step = transaction_->on_event(event);
    return std::visit([&](auto& value) -> concurrency::Action {
        using T = std::decay_t<decltype(value)>;
        core::Result<void> result;
        if constexpr (std::is_same_v<T, TransactionFailure>) {
            return transaction_failure(value.error, value.status);
        } else if constexpr (std::is_same_v<T, TransactionWork>) {
            if (++operation_sequence_ == 0) return close(invalid_config());
            work_ = concurrency::WorkAction{{{id_, requests_served_ + 1}, concurrency::endpoint_role(value.request), operation_sequence_},
                value.deadline, std::move(value.request), cancellation_};
            writing_ = false; waiting_ = false;
            return next_action();
        } else if constexpr (std::is_same_v<T, ResponseHead> || std::is_same_v<T, BufferedResponse>) {
            if (head_started_) return transaction_failure(invalid_config(), StatusCode::InternalServerError);
            head_started_ = true;
            if constexpr (std::is_same_v<T, ResponseHead>) {
                if (parser_.request().version == HttpVersion::http_1_0()) {
                    value.response.version = HttpVersion::http_1_0();
                    if (value.framing == BodyFraming::chunked) {
                        value.framing = BodyFraming::close_delimited;
                        value.response.headers.remove("Transfer-Encoding");
                    }
                }
                if (value.framing == BodyFraming::close_delimited) close_demanded_ = true;
            }
            // Application may demand close but may never override client/limit close.
            for (auto field : value.response.headers.get_all("Connection")) {
                while (!field.empty()) {
                    const auto comma = field.find(',');
                    if (header_name_equals(trim_ws(field.substr(0, comma)), "close")) close_demanded_ = true;
                    if (comma == std::string_view::npos) break;
                    field.remove_prefix(comma + 1);
                }
            }
            value.response.headers.set("Connection", close_demanded_ ? "close" : "keep-alive");
            if (!value.response.headers.contains("Date")) value.response.headers.set("Date", format_http_date(config_.fixed_wall_time.value_or(wall_clock_->now())));
            stats_.response_status = value.response.status_code;
            if constexpr (std::is_same_v<T, ResponseHead>) {
                result = writer_.begin(std::move(value.response), parser_.request().method, value.framing, value.length);
            } else result = writer_.buffered(value.response, parser_.request().method);
        } else if constexpr (std::is_same_v<T, ResponseBody>) {
            result = value.bytes ? writer_.body(*value.bytes) : core::Result<void>(invalid_config());
        } else if constexpr (std::is_same_v<T, ResponseFinished>) {
            result = writer_.finish(value.trailers);
            if (result && writer_.complete()) return finish_response();
        }
        if (!result) return transaction_failure(result.error(), StatusCode::InternalServerError);
        writing_ = true; waiting_ = false;
        stats_.peak_output_bytes = std::max(stats_.peak_output_bytes, writer_.pending().size());
        last_progress_ = steady_clock_->now();
        return next_action();
    }, step);
}

concurrency::Action HttpSession::finish_response() {
    config_.metrics->record(core::Metric::responses_completed);
    config_.metrics->record(core::Metric::body_bytes, writer_.body_bytes());
    if (cancellation_) cancellation_->cancel();
    transaction_.reset(); work_.reset();
    stats_.response_sent = true;
    ++stats_.requests_served; ++requests_served_;
    if (!should_keep_alive()) return close();
    writer_ = ResponseWriter(config_.max_output_bytes);
    head_started_ = false; writing_ = false; waiting_ = false;
    parser_.reset();
    last_progress_ = steady_clock_->now();
    if (!input_.empty()) return process_parsed_input();
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
    if (peer_eof_) {
        return close(core::make_error(core::ErrorCategory::eof, "HTTP request", 0, "Incomplete request at EOF"));
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
        if constexpr (!std::is_same_v<T, StopEvent> && !std::is_same_v<T, WorkEvent>) {
            if (value.id.transaction.sequence != 0 && value.id != downstream_id()) return next_action();
            if (++downstream_sequence_ == 0) return close(core::make_error(core::ErrorCategory::io_error, "HTTP operation sequence exhausted"));
        }
        if constexpr (std::is_same_v<T, WorkEvent>) {
            if (!work_ || value.id != work_->id) return next_action();
            if (value.status == WorkStatus::rejected) config_.metrics->record(core::Metric::work_rejected);
            if (value.status == WorkStatus::cancelled) config_.metrics->record(core::Metric::work_cancelled);
            work_.reset();
            last_progress_ = steady_clock_->now();
            return advance_transaction(value);
        } else if constexpr (std::is_same_v<T, ReadDataEvent>) {
            auto pending = next_action();
            if (pending.is_close()) return pending;
            if (writing_ || transaction_ || value.data.empty() || value.data.size() > read_capacity()) {
                return close(core::make_error(core::ErrorCategory::io_error, "HTTP read", 0, "Invalid read event"));
            }
            waiting_ = false;
            last_progress_ = steady_clock_->now();
            input_.append(reinterpret_cast<const char*>(value.data.data()), value.data.size());
            stats_.peak_input_bytes = std::max(stats_.peak_input_bytes, parser_.buffered_bytes() + input_.size());
            return process_parsed_input();
        } else if constexpr (std::is_same_v<T, ReadEofEvent>) {
            peer_eof_ = true;
            if (writing_ || transaction_) return next_action();
            if (parser_.head_bytes() == 0 && input_.empty()) return close();
            return close(core::make_error(core::ErrorCategory::eof, "HTTP header", 0, "Incomplete header block"));
        } else if constexpr (std::is_same_v<T, WriteCompleteEvent>) {
            auto pending = next_action();
            if (pending.is_close()) return pending;
            if (!writing_) return close(core::make_error(core::ErrorCategory::io_error, "HTTP write"));
            auto ack = writer_.acknowledge(value.bytes_transferred);
            if (!ack) return close(ack.error());
            config_.metrics->record(core::Metric::wire_bytes, value.bytes_transferred);
            last_progress_ = steady_clock_->now(); waiting_ = false;
            if (writer_.pending().empty()) {
                if (writer_.complete()) return finish_response();
                if (!writer_.body_allowed()) {
                    auto finished = writer_.finish();
                    if (!finished) return close(finished.error());
                    return finish_response();
                }
                return advance_transaction(OutputDrained{writer_.body_capacity()});
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
            // A timer from a previous request/phase may arrive after progress.
            return next_action();
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
