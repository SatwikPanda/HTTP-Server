#include "concurrency/single_session_driver.hpp"

#include <algorithm>
#include <thread>
#include <vector>

namespace concurrency {

SingleSessionDriver::SingleSessionDriver(const core::SteadyClock& clock)
    : clock_(&clock) {}

DriverStats SingleSessionDriver::drive(
    net::Socket& socket,
    ConnectionSession& session,
    net::Poller& poller,
    net::RegistrationToken token,
    const std::function<bool()>& stop_requested
) {
    DriverStats stats{};
    std::vector<std::byte> read_buffer;
    Action action = session.initial_action();

    while (!session.is_closed() && !stop_requested()) {
        const auto now = clock_->now();
        if (now - session.last_progress_time() >= session.idle_timeout()) {
            action = session.on_event(TimeoutEvent{});
            stats.error = session.error();
            break;
        }

        if (action.is_close()) {
            stats.error = action.as_close()->error;
            break;
        }

        if (action.is_read()) {
            const std::size_t max_bytes = action.as_read()->max_bytes;
            if (read_buffer.size() < max_bytes) {
                read_buffer.resize(max_bytes);
            }
            auto read_res = socket.read_some(std::span(read_buffer.data(), max_bytes));
            if (read_res.is_ok()) {
                const auto count = read_res.bytes_transferred;
                if (count == 0 || count > max_bytes) {
                    stats.error = core::make_error(core::ErrorCategory::io_error, "driver read", 0, "Invalid progress count");
                    action = session.on_event(ErrorEvent{stats.error});
                    break;
                }
                stats.total_bytes_read += count;
                action = session.on_event(ReadDataEvent{std::span(read_buffer.data(), count)});
            } else if (read_res.is_eof()) {
                action = session.on_event(ReadEofEvent{});
            } else if (read_res.would_block()) {
                ++stats.would_block_read_count;
                action = session.on_event(WouldBlockEvent{net::Interest::read});
            } else {
                stats.error = read_res.error;
                action = session.on_event(ErrorEvent{read_res.error});
                break;
            }
        } else if (action.is_write()) {
            const auto data = action.as_write()->data;
            if (data.empty()) {
                action = session.on_event(WriteCompleteEvent{0});
                continue;
            }
            auto write_res = socket.write_some(data);
            if (write_res.is_ok()) {
                const auto count = write_res.bytes_transferred;
                if (count == 0 || count > data.size()) {
                    stats.error = core::make_error(core::ErrorCategory::io_error, "driver write", 0, "Invalid progress count");
                    action = session.on_event(ErrorEvent{stats.error});
                    break;
                }
                if (count < data.size()) {
                    ++stats.short_write_count;
                }
                stats.total_bytes_written += count;
                action = session.on_event(WriteCompleteEvent{count});
            } else if (write_res.would_block()) {
                ++stats.would_block_write_count;
                action = session.on_event(WouldBlockEvent{net::Interest::write});
            } else {
                stats.error = write_res.error;
                action = session.on_event(ErrorEvent{write_res.error});
                break;
            }
        } else if (action.is_wait()) {
            const auto interest = action.as_wait()->interest;
            const auto deadline = action.as_wait()->deadline;
            auto modified = poller.modify(token, interest);
            if (!modified) {
                stats.error = modified.error();
                action = session.on_event(ErrorEvent{stats.error});
                break;
            }

            bool got_event = false;
            while (!stop_requested() && !got_event) {
                ++stats.readiness_wait_count;
                auto events_res = poller.wait(deadline);
                if (!events_res) {
                    stats.error = events_res.error();
                    action = session.on_event(ErrorEvent{stats.error});
                    break;
                }
                if (clock_->now() >= deadline) {
                    action = session.on_event(TimeoutEvent{});
                    stats.error = session.error();
                    break;
                }
                for (const auto& event : events_res.value()) {
                    if (event.token == token && poller.is_current(event)) {
                        got_event = true;
                        action = session.on_event(ReadyEvent{event.readable || event.hangup, event.writable});
                        break;
                    }
                }
            }
        } else {
            break;
        }
    }

    if (stop_requested() && !session.is_closed()) {
        action = session.on_event(StopEvent{std::chrono::milliseconds(0)});
    }

    stats.completed_cleanly = session.is_closed() && !stats.error && !session.error();
    if (!stats.error && session.error()) {
        stats.error = session.error();
    }
    return stats;
}

DriverStats SingleSessionDriver::drive_channel(
    net::StreamChannel& channel,
    ConnectionSession& session,
    std::chrono::milliseconds wait_retry_delay,
    const std::function<bool()>& stop_requested
) {
    DriverStats stats{};
    std::vector<std::byte> read_buffer;
    Action action = session.initial_action();

    while (channel.is_valid() && !session.is_closed() && !stop_requested()) {
        const auto now = clock_->now();
        if (now - session.last_progress_time() >= session.idle_timeout()) {
            action = session.on_event(TimeoutEvent{});
            stats.error = session.error();
            break;
        }

        if (action.is_close()) {
            stats.error = action.as_close()->error;
            break;
        }

        if (action.is_read()) {
            const std::size_t max_bytes = action.as_read()->max_bytes;
            if (read_buffer.size() < max_bytes) {
                read_buffer.resize(max_bytes);
            }
            auto read_res = channel.read_some(std::span(read_buffer.data(), max_bytes));
            if (read_res.is_ok()) {
                const auto count = read_res.bytes_transferred;
                if (count == 0 || count > max_bytes) {
                    stats.error = core::make_error(core::ErrorCategory::io_error, "channel read", 0, "Invalid progress count");
                    action = session.on_event(ErrorEvent{stats.error});
                    break;
                }
                stats.total_bytes_read += count;
                action = session.on_event(ReadDataEvent{std::span(read_buffer.data(), count)});
            } else if (read_res.is_eof()) {
                action = session.on_event(ReadEofEvent{});
            } else if (read_res.would_block()) {
                ++stats.would_block_read_count;
                action = session.on_event(WouldBlockEvent{net::Interest::read});
            } else {
                stats.error = read_res.error;
                action = session.on_event(ErrorEvent{read_res.error});
                break;
            }
        } else if (action.is_write()) {
            const auto data = action.as_write()->data;
            if (data.empty()) {
                action = session.on_event(WriteCompleteEvent{0});
                continue;
            }
            auto write_res = channel.write_some(data);
            if (write_res.is_ok()) {
                const auto count = write_res.bytes_transferred;
                if (count == 0 || count > data.size()) {
                    stats.error = core::make_error(core::ErrorCategory::io_error, "channel write", 0, "Invalid progress count");
                    action = session.on_event(ErrorEvent{stats.error});
                    break;
                }
                if (count < data.size()) {
                    ++stats.short_write_count;
                }
                stats.total_bytes_written += count;
                action = session.on_event(WriteCompleteEvent{count});
            } else if (write_res.would_block()) {
                ++stats.would_block_write_count;
                action = session.on_event(WouldBlockEvent{net::Interest::write});
            } else {
                stats.error = write_res.error;
                action = session.on_event(ErrorEvent{write_res.error});
                break;
            }
        } else if (action.is_wait()) {
            const auto deadline = action.as_wait()->deadline;
            const auto current_time = clock_->now();
            if (current_time >= deadline) {
                action = session.on_event(TimeoutEvent{});
                stats.error = session.error();
                break;
            }

            const auto retry_at = std::min(deadline, current_time + wait_retry_delay);
            auto remaining = retry_at - current_time;
            while (remaining > std::chrono::steady_clock::duration::zero()) {
                std::this_thread::sleep_for(std::chrono::ceil<std::chrono::milliseconds>(remaining));
                remaining = retry_at - clock_->now();
            }

            if (clock_->now() >= deadline) {
                action = session.on_event(TimeoutEvent{});
                stats.error = session.error();
                break;
            }

            action = session.on_event(ReadyEvent{true, true});
        } else {
            break;
        }
    }

    stats.completed_cleanly = session.is_closed() && !stats.error && !session.error();
    if (!stats.error && session.error()) {
        stats.error = session.error();
    }
    return stats;
}

} // namespace concurrency
