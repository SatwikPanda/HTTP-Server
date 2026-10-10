#pragma once

#include "concurrency/session.hpp"
#include "concurrency/single_session_driver.hpp"
#include "core/clock.hpp"
#include "net/endpoint.hpp"
#include "net/poller.hpp"
#include "net/runtime.hpp"
#include "net/socket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace tests {

class SessionHarness {
public:
    SessionHarness(std::size_t input_limit, std::size_t output_limit,
                   std::chrono::milliseconds idle_timeout)
        : idle_timeout_(idle_timeout) {
        core::ConnectionId id{1, 1};
        session_ = std::make_unique<concurrency::EchoSession>(
            id, input_limit, output_limit, idle_timeout, clock_);
        current_action_ = session_->initial_action();
    }

    void force_write_chunk(std::size_t chunk) {
        force_write_chunk_ = chunk;
    }

    void block_next_writes(std::size_t count) {
        block_next_writes_ = count;
    }

    void receive(std::span<const std::byte> data) {
        incoming_data_.insert(incoming_data_.end(), data.begin(), data.end());
        step_feed();
    }

    void peer_eof() {
        peer_eof_ = true;
        step_feed();
    }

    void run_until_idle() {
        bool progress = true;
        while (progress && !closed_) {
            progress = step_progress();
        }
    }

    void advance(std::chrono::milliseconds ms) {
        clock_.advance(ms);
        check_deadlines();
        if (!closed_) {
            step_feed();
        }
    }

    void stop(std::chrono::milliseconds drain_timeout) {
        stopped_ = true;
        drain_deadline_ = clock_.now() + drain_timeout;
        current_action_ = session_->on_event(concurrency::StopEvent{drain_timeout});
        if (session_->is_closed() || current_action_.is_close()) {
            closed_ = true;
        }
    }

    [[nodiscard]] const std::vector<std::byte>& output() const {
        return outgoing_data_;
    }

    [[nodiscard]] bool closed() const {
        return closed_ || session_->is_closed();
    }

    [[nodiscard]] std::size_t peak_input_bytes() const {
        return session_->peak_input_bytes();
    }

    [[nodiscard]] std::size_t peak_output_bytes() const {
        return session_->peak_output_bytes();
    }

    [[nodiscard]] std::size_t open_resources() const {
        return closed() ? 0 : 1;
    }

private:
    void check_deadlines() {
        if (closed_) return;
        const auto now = clock_.now();
        if (stopped_ && drain_deadline_ != std::chrono::steady_clock::time_point{}) {
            if (now >= drain_deadline_) {
                closed_ = true;
                return;
            }
        }
        if (now - session_->last_progress_time() >= idle_timeout_) {
            current_action_ = session_->on_event(concurrency::TimeoutEvent{});
            closed_ = true;
            return;
        }
    }

    void step_feed() {
        if (closed_) return;
        check_deadlines();
        if (closed_) return;

        while (current_action_.is_read() && !closed_) {
            const std::size_t max_bytes = current_action_.as_read()->max_bytes;
            const std::size_t avail = incoming_data_.size() - incoming_offset_;
            if (avail > 0) {
                const std::size_t to_read = std::min(max_bytes, avail);
                auto span = std::span(incoming_data_.data() + incoming_offset_, to_read);
                incoming_offset_ += to_read;
                current_action_ = session_->on_event(concurrency::ReadDataEvent{span});
            } else if (peer_eof_) {
                current_action_ = session_->on_event(concurrency::ReadEofEvent{});
            } else {
                break;
            }
        }

        if (current_action_.is_write() && !closed_) {
            auto data = current_action_.as_write()->data;
            if (block_next_writes_ > 0) {
                --block_next_writes_;
                current_action_ = session_->on_event(concurrency::WouldBlockEvent{net::Interest::write});
            } else {
                std::size_t to_write = data.size();
                if (force_write_chunk_ > 0 && to_write > force_write_chunk_) {
                    to_write = force_write_chunk_;
                }
                outgoing_data_.insert(outgoing_data_.end(), data.data(), data.data() + to_write);
                current_action_ = session_->on_event(concurrency::WriteCompleteEvent{to_write});
            }
        }

        if (current_action_.is_close() || session_->is_closed()) {
            closed_ = true;
        }
    }

    bool step_progress() {
        if (closed_) return false;
        check_deadlines();
        if (closed_) return false;

        bool made_progress = false;

        if (current_action_.is_wait()) {
            const auto interest = current_action_.as_wait()->interest;
            if (static_cast<unsigned>(interest) & static_cast<unsigned>(net::Interest::write)) {
                current_action_ = session_->on_event(concurrency::ReadyEvent{false, true});
                made_progress = true;
            } else if ((static_cast<unsigned>(interest) & static_cast<unsigned>(net::Interest::read)) &&
                       (incoming_offset_ < incoming_data_.size() || peer_eof_)) {
                current_action_ = session_->on_event(concurrency::ReadyEvent{true, false});
                made_progress = true;
            } else {
                return false;
            }
        }

        if (current_action_.is_read()) {
            const std::size_t max_bytes = current_action_.as_read()->max_bytes;
            const std::size_t avail = incoming_data_.size() - incoming_offset_;
            if (avail > 0) {
                const std::size_t to_read = std::min(max_bytes, avail);
                auto span = std::span(incoming_data_.data() + incoming_offset_, to_read);
                incoming_offset_ += to_read;
                current_action_ = session_->on_event(concurrency::ReadDataEvent{span});
                made_progress = true;
            } else if (peer_eof_) {
                current_action_ = session_->on_event(concurrency::ReadEofEvent{});
                made_progress = true;
            }
        }

        if (current_action_.is_write()) {
            auto data = current_action_.as_write()->data;
            if (block_next_writes_ > 0) {
                --block_next_writes_;
                current_action_ = session_->on_event(concurrency::WouldBlockEvent{net::Interest::write});
                made_progress = true;
            } else {
                std::size_t to_write = data.size();
                if (force_write_chunk_ > 0 && to_write > force_write_chunk_) {
                    to_write = force_write_chunk_;
                }
                outgoing_data_.insert(outgoing_data_.end(), data.data(), data.data() + to_write);
                current_action_ = session_->on_event(concurrency::WriteCompleteEvent{to_write});
                made_progress = true;
            }
        }

        if (current_action_.is_close() || session_->is_closed()) {
            closed_ = true;
            made_progress = true;
        }

        return made_progress;
    }

    core::ManualSteadyClock clock_;
    std::unique_ptr<concurrency::EchoSession> session_;
    std::vector<std::byte> incoming_data_;
    std::size_t incoming_offset_{0};
    bool peer_eof_{false};
    std::vector<std::byte> outgoing_data_;
    std::size_t force_write_chunk_{0};
    std::size_t block_next_writes_{0};
    bool stopped_{false};
    std::chrono::milliseconds idle_timeout_{0};
    std::chrono::steady_clock::time_point drain_deadline_{};
    bool closed_{false};
    concurrency::Action current_action_;
};

inline bool echo_session_real_socket_roundtrip(std::span<const std::byte> payload) {
    auto runtime_res = net::initialize_network();
    if (!runtime_res) return false;

    auto socket_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    if (!socket_res) return false;
    auto listener = std::move(socket_res.value());
    if (!listener.bind(net::Endpoint::ipv4_loopback(0))) return false;
    if (!listener.listen(128)) return false;
    if (!listener.set_nonblocking(true)) return false;
    auto local_ep_res = listener.local_endpoint();
    if (!local_ep_res) return false;
    const auto local_ep = local_ep_res.value();

    auto poller_res = net::Poller::create();
    if (!poller_res) return false;
    auto poller = std::move(poller_res.value());
    auto listener_watched = poller.watch(listener, net::Interest::read, {0, 1});
    if (!listener_watched) return false;
    const auto listener_token = listener_watched.value();

    std::vector<std::byte> received;
    std::atomic<bool> client_ok{false};

    std::thread client_thread([&] {
        auto client_sock_res = net::create_tcp_socket(net::AddressFamily::ipv4);
        if (!client_sock_res) return;
        auto client = std::move(client_sock_res.value());
        if (!client.connect(local_ep)) return;

        std::size_t sent = 0;
        while (sent < payload.size()) {
            auto res = client.write_some(payload.subspan(sent));
            if (res.is_ok()) {
                sent += res.bytes_transferred;
            } else if (res.would_block()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                return;
            }
        }
        if (!client.shutdown_write()) return;

        std::byte buf[256];
        while (true) {
            auto res = client.read_some(std::span(buf));
            if (res.is_ok()) {
                received.insert(received.end(), buf, buf + res.bytes_transferred);
            } else if (res.is_eof()) {
                break;
            } else if (res.would_block()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                return;
            }
        }
        client.close();
        if (received.size() == payload.size() &&
            std::memcmp(received.data(), payload.data(), payload.size()) == 0) {
            client_ok.store(true);
        }
    });

    bool server_ok = false;
    auto ready = poller.wait(std::chrono::steady_clock::now() + std::chrono::seconds(5));
    if (ready && !ready.value().empty()) {
        auto accepted = listener.accept();
        if (accepted) {
            auto client = std::move(accepted.value());
            (void)client.set_nonblocking(true);
            auto client_watched = poller.watch(client, net::Interest::read, {1, 2});
            if (client_watched) {
                (void)poller.modify(listener_token, net::Interest::none);
                concurrency::SingleSessionDriver driver;
                concurrency::EchoSession session(
                    {1, 2}, 128, 128, std::chrono::seconds(5));
                auto stats = driver.drive(client, session, poller, client_watched.value());
                (void)poller.unwatch(client_watched.value());
                client.close();
                server_ok = stats.completed_cleanly;
            }
        }
    }

    client_thread.join();
    (void)poller.unwatch(listener_token);
    listener.close();
    return server_ok && client_ok.load();
}

inline bool driver_accepts_two_sequential_clients() {
    auto runtime_res = net::initialize_network();
    if (!runtime_res) return false;

    auto socket_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    if (!socket_res) return false;
    auto listener = std::move(socket_res.value());
    if (!listener.bind(net::Endpoint::ipv4_loopback(0))) return false;
    if (!listener.listen(128)) return false;
    if (!listener.set_nonblocking(true)) return false;
    auto local_ep_res = listener.local_endpoint();
    if (!local_ep_res) return false;
    const auto local_ep = local_ep_res.value();

    auto poller_res = net::Poller::create();
    if (!poller_res) return false;
    auto poller = std::move(poller_res.value());
    auto listener_watched = poller.watch(listener, net::Interest::read, {0, 1});
    if (!listener_watched) return false;
    const auto listener_token = listener_watched.value();

    for (int i = 0; i < 2; ++i) {
        std::atomic<bool> client_ok{false};
        std::thread client_thread([&, i] {
            auto client_sock_res = net::create_tcp_socket(net::AddressFamily::ipv4);
            if (!client_sock_res) return;
            auto client = std::move(client_sock_res.value());
            if (!client.connect(local_ep)) return;

            std::string msg = "hello client " + std::to_string(i);
            std::span<const std::byte> send_bytes(
                reinterpret_cast<const std::byte*>(msg.data()), msg.size());

            std::size_t sent = 0;
            while (sent < send_bytes.size()) {
                auto res = client.write_some(send_bytes.subspan(sent));
                if (res.is_ok()) sent += res.bytes_transferred;
                else if (res.would_block()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                else return;
            }
            if (!client.shutdown_write()) return;

            std::string recvd;
            char buf[64];
            while (true) {
                auto res = client.read_some(std::span(reinterpret_cast<std::byte*>(buf), sizeof(buf)));
                if (res.is_ok()) recvd.append(buf, res.bytes_transferred);
                else if (res.is_eof()) break;
                else if (res.would_block()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                else return;
            }
            client.close();
            if (recvd == msg) client_ok.store(true);
        });

        bool server_ok = false;
        auto ready = poller.wait(std::chrono::steady_clock::now() + std::chrono::seconds(5));
        if (ready && !ready.value().empty()) {
            auto accepted = listener.accept();
            if (accepted) {
                auto client = std::move(accepted.value());
                (void)client.set_nonblocking(true);
                auto client_watched = poller.watch(client, net::Interest::read, {1, static_cast<std::uint64_t>(i + 2)});
                if (client_watched) {
                    (void)poller.modify(listener_token, net::Interest::none);
                    concurrency::SingleSessionDriver driver;
                    concurrency::EchoSession session(
                        {1, static_cast<std::uint64_t>(i + 2)}, 256, 256, std::chrono::seconds(5));
                    auto stats = driver.drive(client, session, poller, client_watched.value());
                    (void)poller.unwatch(client_watched.value());
                    client.close();
                    server_ok = stats.completed_cleanly;
                    (void)poller.modify(listener_token, net::Interest::read);
                }
            }
        }

        client_thread.join();
        if (!server_ok || !client_ok.load()) {
            (void)poller.unwatch(listener_token);
            listener.close();
            return false;
        }
    }

    (void)poller.unwatch(listener_token);
    listener.close();
    return true;
}

} // namespace tests
