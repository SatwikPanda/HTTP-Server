#pragma once

#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/id.hpp"
#include "net/poller.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace concurrency {

enum class SessionState {
    initial,
    reading,
    writing,
    waiting,
    draining,
    closed
};

struct ReadAction {
    std::size_t max_bytes{0};
    bool operator==(const ReadAction&) const = default;
};

struct WriteAction {
    std::span<const std::byte> data{};
};

struct WaitAction {
    net::Interest interest{net::Interest::none};
    std::chrono::steady_clock::time_point deadline{};
};

struct CloseAction {
    core::Error error{};
};

class Action {
public:
    using Storage = std::variant<std::monostate, ReadAction, WriteAction, WaitAction, CloseAction>;

    Action() noexcept = default;
    Action(ReadAction a) noexcept : storage_(a) {}
    Action(WriteAction a) noexcept : storage_(a) {}
    Action(WaitAction a) noexcept : storage_(a) {}
    Action(CloseAction a) noexcept : storage_(std::move(a)) {}

    [[nodiscard]] bool is_none() const noexcept { return std::holds_alternative<std::monostate>(storage_); }
    [[nodiscard]] bool is_read() const noexcept { return std::holds_alternative<ReadAction>(storage_); }
    [[nodiscard]] bool is_write() const noexcept { return std::holds_alternative<WriteAction>(storage_); }
    [[nodiscard]] bool is_wait() const noexcept { return std::holds_alternative<WaitAction>(storage_); }
    [[nodiscard]] bool is_close() const noexcept { return std::holds_alternative<CloseAction>(storage_); }

    [[nodiscard]] const ReadAction* as_read() const noexcept { return std::get_if<ReadAction>(&storage_); }
    [[nodiscard]] const WriteAction* as_write() const noexcept { return std::get_if<WriteAction>(&storage_); }
    [[nodiscard]] const WaitAction* as_wait() const noexcept { return std::get_if<WaitAction>(&storage_); }
    [[nodiscard]] const CloseAction* as_close() const noexcept { return std::get_if<CloseAction>(&storage_); }

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

    template <class Visitor>
    decltype(auto) visit(Visitor&& vis) const {
        return std::visit(std::forward<Visitor>(vis), storage_);
    }

private:
    Storage storage_{std::monostate{}};
};

struct ReadDataEvent {
    std::span<const std::byte> data;
};

struct ReadEofEvent {};

struct WriteCompleteEvent {
    std::size_t bytes_transferred{0};
};

struct ReadyEvent {
    bool readable{false};
    bool writable{false};
};

struct WouldBlockEvent {
    net::Interest interest{net::Interest::none};
};

struct TimeoutEvent {};

struct StopEvent {
    std::chrono::milliseconds drain_timeout{0};
};

struct ErrorEvent {
    core::Error error;
};

class Event {
public:
    using Storage = std::variant<
        ReadDataEvent,
        ReadEofEvent,
        WriteCompleteEvent,
        ReadyEvent,
        WouldBlockEvent,
        TimeoutEvent,
        StopEvent,
        ErrorEvent
    >;

    template <typename T>
    Event(T&& val) noexcept : storage_(std::forward<T>(val)) {}

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

    template <class Visitor>
    decltype(auto) visit(Visitor&& vis) const {
        return std::visit(std::forward<Visitor>(vis), storage_);
    }

private:
    Storage storage_;
};

class ConnectionSession {
public:
    virtual ~ConnectionSession() = default;

    [[nodiscard]] virtual Action on_event(const Event& event) = 0;
    [[nodiscard]] virtual Action initial_action() = 0;

    [[nodiscard]] virtual SessionState state() const noexcept = 0;
    [[nodiscard]] virtual bool is_closed() const noexcept = 0;

    [[nodiscard]] virtual std::size_t input_limit() const noexcept = 0;
    [[nodiscard]] virtual std::size_t output_limit() const noexcept = 0;
    [[nodiscard]] virtual std::size_t peak_input_bytes() const noexcept = 0;
    [[nodiscard]] virtual std::size_t peak_output_bytes() const noexcept = 0;

    [[nodiscard]] virtual core::ConnectionId connection_id() const noexcept = 0;
    [[nodiscard]] virtual std::chrono::milliseconds idle_timeout() const noexcept = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point last_progress_time() const noexcept = 0;
    [[nodiscard]] virtual const core::Error& error() const noexcept = 0;
};

class EchoSession final : public ConnectionSession {
public:
    EchoSession(
        core::ConnectionId id,
        std::size_t input_limit,
        std::size_t output_limit,
        std::chrono::milliseconds idle_timeout,
        const core::SteadyClock& clock = core::real_steady_clock()
    );
    ~EchoSession() override = default;

    [[nodiscard]] Action on_event(const Event& event) override;
    [[nodiscard]] Action initial_action() override;

    [[nodiscard]] SessionState state() const noexcept override { return state_; }
    [[nodiscard]] bool is_closed() const noexcept override { return closed_; }

    [[nodiscard]] std::size_t input_limit() const noexcept override { return input_limit_; }
    [[nodiscard]] std::size_t output_limit() const noexcept override { return output_limit_; }
    [[nodiscard]] std::size_t peak_input_bytes() const noexcept override { return peak_input_; }
    [[nodiscard]] std::size_t peak_output_bytes() const noexcept override { return peak_output_; }

    [[nodiscard]] core::ConnectionId connection_id() const noexcept override { return id_; }
    [[nodiscard]] std::chrono::milliseconds idle_timeout() const noexcept override { return idle_timeout_; }
    [[nodiscard]] std::chrono::steady_clock::time_point last_progress_time() const noexcept override { return last_progress_; }
    [[nodiscard]] const core::Error& error() const noexcept override { return error_; }

    [[nodiscard]] bool has_pending_output() const noexcept {
        return !output_buffer_.empty() || !input_buffer_.empty();
    }

    [[nodiscard]] bool is_stopping() const noexcept { return stopping_; }
    [[nodiscard]] std::chrono::steady_clock::time_point drain_deadline() const noexcept { return drain_deadline_; }

private:
    void move_input_to_output();
    [[nodiscard]] net::Interest compute_desired_interest() const noexcept;

    core::ConnectionId id_;
    std::size_t input_limit_{0};
    std::size_t output_limit_{0};
    std::chrono::milliseconds idle_timeout_{0};
    const core::SteadyClock* clock_{nullptr};

    std::vector<std::byte> input_buffer_;
    std::vector<std::byte> output_buffer_;

    std::size_t peak_input_{0};
    std::size_t peak_output_{0};

    std::chrono::steady_clock::time_point last_progress_{};
    std::chrono::steady_clock::time_point drain_deadline_{};

    SessionState state_{SessionState::initial};
    bool peer_eof_{false};
    bool stopping_{false};
    bool closed_{false};
    core::Error error_{};
};

} // namespace concurrency
