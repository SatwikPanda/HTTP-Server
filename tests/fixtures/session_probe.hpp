#pragma once

#include "concurrency/session.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace test {

// Observe real driver I/O without replacing the production session's actions,
// deadlines or state transitions. The probe must outlive the driver.
struct SessionProbe {
    std::atomic<std::size_t> bytes_written{};
    std::atomic<std::size_t> blocked_writes{};
    std::atomic<bool> stopped{};
    std::atomic<bool> timed_out{};
};

class ProbedSession final : public concurrency::ConnectionSession {
public:
    ProbedSession(std::unique_ptr<concurrency::ConnectionSession> session, SessionProbe& probe)
        : session_(std::move(session)), probe_(probe) {}

    concurrency::Action initial_action() override { return session_->initial_action(); }
    concurrency::Action on_event(const concurrency::Event& event) override {
        auto action = session_->on_event(event);
        if (const auto* write = std::get_if<concurrency::WriteCompleteEvent>(&event.storage())) {
            probe_.bytes_written.fetch_add(write->bytes_transferred);
        }
        if (const auto* blocked = std::get_if<concurrency::WouldBlockEvent>(&event.storage());
            blocked && blocked->interest == net::Interest::write) {
            probe_.blocked_writes.fetch_add(1);
        }
        if (std::holds_alternative<concurrency::StopEvent>(event.storage())) probe_.stopped = true;
        if (session_->is_closed() && session_->error().category == core::ErrorCategory::timed_out) {
            probe_.timed_out = true;
        }
        return action;
    }

    concurrency::SessionState state() const noexcept override { return session_->state(); }
    bool is_closed() const noexcept override { return session_->is_closed(); }
    std::size_t input_limit() const noexcept override { return session_->input_limit(); }
    std::size_t output_limit() const noexcept override { return session_->output_limit(); }
    std::size_t peak_input_bytes() const noexcept override { return session_->peak_input_bytes(); }
    std::size_t peak_output_bytes() const noexcept override { return session_->peak_output_bytes(); }
    core::ConnectionId connection_id() const noexcept override { return session_->connection_id(); }
    std::chrono::milliseconds idle_timeout() const noexcept override { return session_->idle_timeout(); }
    core::SteadyTimePoint last_progress_time() const noexcept override { return session_->last_progress_time(); }
    const core::Error& error() const noexcept override { return session_->error(); }

private:
    std::unique_ptr<concurrency::ConnectionSession> session_;
    SessionProbe& probe_;
};

} // namespace test
