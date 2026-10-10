#include "concurrency/session.hpp"

#include <algorithm>

namespace concurrency {

EchoSession::EchoSession(
    core::ConnectionId id,
    std::size_t input_limit,
    std::size_t output_limit,
    std::chrono::milliseconds idle_timeout,
    const core::SteadyClock& clock
)
    : id_(id),
      input_limit_(std::max<std::size_t>(input_limit, 1)),
      output_limit_(std::max<std::size_t>(output_limit, 1)),
      idle_timeout_(idle_timeout),
      clock_(&clock),
      last_progress_(clock.now()) {}

void EchoSession::move_input_to_output() {
    const std::size_t space = output_limit_ > output_buffer_.size()
        ? output_limit_ - output_buffer_.size() : 0;
    const std::size_t to_move = std::min(space, input_buffer_.size());
    if (to_move > 0) {
        output_buffer_.insert(
            output_buffer_.end(),
            input_buffer_.begin(),
            input_buffer_.begin() + static_cast<std::ptrdiff_t>(to_move)
        );
        input_buffer_.erase(
            input_buffer_.begin(),
            input_buffer_.begin() + static_cast<std::ptrdiff_t>(to_move)
        );
        peak_output_ = std::max(peak_output_, output_buffer_.size());
    }
}

net::Interest EchoSession::compute_desired_interest() const noexcept {
    unsigned interest = static_cast<unsigned>(net::Interest::none);
    if (!output_buffer_.empty()) {
        interest |= static_cast<unsigned>(net::Interest::write);
    }
    if (!peer_eof_ && !stopping_ && input_buffer_.size() < input_limit_) {
        interest |= static_cast<unsigned>(net::Interest::read);
    }
    return static_cast<net::Interest>(interest);
}

Action EchoSession::initial_action() {
    last_progress_ = clock_->now();
    state_ = SessionState::reading;
    return ReadAction{input_limit_};
}

Action EchoSession::on_event(const Event& event) {
    if (closed_) {
        return CloseAction{error_};
    }

    return event.visit([this](const auto& e) -> Action {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, ReadDataEvent>) {
            last_progress_ = clock_->now();
            input_buffer_.insert(input_buffer_.end(), e.data.begin(), e.data.end());
            peak_input_ = std::max(peak_input_, input_buffer_.size());
            move_input_to_output();

            if (!output_buffer_.empty()) {
                state_ = stopping_ ? SessionState::draining : SessionState::writing;
                return WriteAction{std::span(output_buffer_)};
            }
            if (input_buffer_.size() < input_limit_ && !peer_eof_ && !stopping_) {
                state_ = SessionState::reading;
                return ReadAction{input_limit_ - input_buffer_.size()};
            }
            state_ = SessionState::waiting;
            return WaitAction{compute_desired_interest(), last_progress_ + idle_timeout_};
        } else if constexpr (std::is_same_v<T, WriteCompleteEvent>) {
            last_progress_ = clock_->now();
            const std::size_t count = std::min(e.bytes_transferred, output_buffer_.size());
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + static_cast<std::ptrdiff_t>(count));
            move_input_to_output();

            if (!output_buffer_.empty()) {
                state_ = stopping_ ? SessionState::draining : SessionState::writing;
                return WriteAction{std::span(output_buffer_)};
            }
            if ((peer_eof_ && input_buffer_.empty()) || (stopping_ && input_buffer_.empty())) {
                closed_ = true;
                state_ = SessionState::closed;
                return CloseAction{};
            }
            if (input_buffer_.size() < input_limit_ && !peer_eof_ && !stopping_) {
                state_ = SessionState::reading;
                return ReadAction{input_limit_ - input_buffer_.size()};
            }
            state_ = SessionState::waiting;
            return WaitAction{compute_desired_interest(), last_progress_ + idle_timeout_};
        } else if constexpr (std::is_same_v<T, ReadEofEvent>) {
            peer_eof_ = true;
            move_input_to_output();
            if (!output_buffer_.empty()) {
                state_ = stopping_ ? SessionState::draining : SessionState::writing;
                return WriteAction{std::span(output_buffer_)};
            }
            closed_ = true;
            state_ = SessionState::closed;
            return CloseAction{};
        } else if constexpr (std::is_same_v<T, WouldBlockEvent>) {
            state_ = SessionState::waiting;
            const auto deadline = (stopping_ && drain_deadline_ != std::chrono::steady_clock::time_point{})
                ? drain_deadline_
                : (last_progress_ + idle_timeout_);
            return WaitAction{e.interest, deadline};
        } else if constexpr (std::is_same_v<T, ReadyEvent>) {
            if (e.writable && !output_buffer_.empty()) {
                state_ = stopping_ ? SessionState::draining : SessionState::writing;
                return WriteAction{std::span(output_buffer_)};
            }
            if (e.readable && !peer_eof_ && !stopping_ && input_buffer_.size() < input_limit_) {
                state_ = SessionState::reading;
                return ReadAction{input_limit_ - input_buffer_.size()};
            }
            if (output_buffer_.empty() && input_buffer_.empty() && (peer_eof_ || stopping_)) {
                closed_ = true;
                state_ = SessionState::closed;
                return CloseAction{};
            }
            state_ = SessionState::waiting;
            const auto deadline = (stopping_ && drain_deadline_ != std::chrono::steady_clock::time_point{})
                ? drain_deadline_
                : (last_progress_ + idle_timeout_);
            return WaitAction{compute_desired_interest(), deadline};
        } else if constexpr (std::is_same_v<T, TimeoutEvent>) {
            closed_ = true;
            state_ = SessionState::closed;
            error_ = core::make_error(core::ErrorCategory::timed_out, "echo session timeout");
            return CloseAction{error_};
        } else if constexpr (std::is_same_v<T, StopEvent>) {
            stopping_ = true;
            drain_deadline_ = clock_->now() + e.drain_timeout;
            move_input_to_output();
            if (output_buffer_.empty() && input_buffer_.empty()) {
                closed_ = true;
                state_ = SessionState::closed;
                return CloseAction{};
            }
            state_ = SessionState::draining;
            return WriteAction{std::span(output_buffer_)};
        } else if constexpr (std::is_same_v<T, ErrorEvent>) {
            closed_ = true;
            state_ = SessionState::closed;
            error_ = e.error;
            return CloseAction{error_};
        }
    });
}

} // namespace concurrency
