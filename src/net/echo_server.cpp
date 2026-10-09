#include "net/echo_server.hpp"

#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

namespace net {

EchoSessionStats run_echo_session(
    StreamChannel& channel,
    const EchoConfig& config
) {
    EchoSessionStats stats{};

    std::size_t capacity = std::max<std::size_t>(config.buffer_capacity, 1);
    std::vector<std::byte> buffer(capacity);

    std::size_t bytes_in_buffer = 0;
    std::size_t unsent_offset = 0;
    bool client_eof = false;

    auto last_progress_time = std::chrono::steady_clock::now();

    while (channel.is_valid()) {
        // 1. If we have buffered data not yet sent, send it out.
        if (unsent_offset < bytes_in_buffer) {
            std::span<const std::byte> to_send(
                buffer.data() + unsent_offset,
                bytes_in_buffer - unsent_offset
            );

            WriteResult write_res = channel.write_some(to_send);

            if (write_res.is_ok()) {
                std::size_t sent = write_res.bytes_transferred;
                if (sent < to_send.size()) {
                    stats.short_write_count++;
                }
                unsent_offset += sent;
                stats.total_bytes_written += sent;
                last_progress_time = std::chrono::steady_clock::now();

                if (unsent_offset == bytes_in_buffer) {
                    bytes_in_buffer = 0;
                    unsent_offset = 0;

                    if (client_eof) {
                        stats.completed_cleanly = true;
                        break;
                    }
                }
                continue;
            }

            if (write_res.would_block()) {
                stats.would_block_write_count++;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_progress_time
                );
                if (elapsed >= config.retry_timeout) {
                    break; // Timed out waiting for client to accept output
                }
                std::this_thread::sleep_for(config.wait_retry_delay);
                continue;
            }

            if (write_res.is_error()) {
                // Socket error on write
                break;
            }
        }

        // 2. Buffer is empty.
        if (client_eof) {
            stats.completed_cleanly = true;
            break;
        }

        std::span<std::byte> to_read(buffer.data(), buffer.size());
        ReadResult read_res = channel.read_some(to_read);

        if (read_res.is_ok()) {
            bytes_in_buffer = read_res.bytes_transferred;
            unsent_offset = 0;
            stats.total_bytes_read += bytes_in_buffer;
            last_progress_time = std::chrono::steady_clock::now();
            continue;
        }

        if (read_res.is_eof()) {
            client_eof = true;
            if (bytes_in_buffer == unsent_offset) {
                stats.completed_cleanly = true;
                break;
            }
            continue;
        }

        if (read_res.would_block()) {
            stats.would_block_read_count++;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_progress_time
            );
            if (elapsed >= config.retry_timeout) {
                break; // Idle read timeout
            }
            std::this_thread::sleep_for(config.wait_retry_delay);
            continue;
        }

        if (read_res.is_error()) {
            // Socket error on read
            break;
        }
    }

    return stats;
}

EchoSessionStats run_echo_session(
    Socket& socket,
    const EchoConfig& config
) {
    SocketStreamChannel channel(socket);
    return run_echo_session(channel, config);
}

EchoServer::EchoServer(EchoConfig config)
    : config_(std::move(config)) {}

EchoServer::~EchoServer() {
    stop();
}

core::Result<void> EchoServer::start() {
    auto sock_res = create_tcp_socket(config_.endpoint.family());
    if (!sock_res) {
        return sock_res.error();
    }
    listener_ = std::move(sock_res.value());

    auto reuse_res = listener_.set_reuse_address(true);
    if (!reuse_res) {
        listener_.close();
        return reuse_res.error();
    }

    auto bind_res = listener_.bind(config_.endpoint);
    if (!bind_res) {
        listener_.close();
        return bind_res.error();
    }

    auto listen_res = listener_.listen(128);
    if (!listen_res) {
        listener_.close();
        return listen_res.error();
    }

    auto local_res = listener_.local_endpoint();
    if (local_res) {
        actual_endpoint_ = local_res.value();
    } else {
        actual_endpoint_ = config_.endpoint;
    }

    running_ = true;
    return core::Result<void>::success();
}

void EchoServer::stop() noexcept {
    running_ = false;
    listener_.close();
}

bool EchoServer::is_running() const noexcept {
    return running_ && listener_.is_valid();
}

core::Result<void> EchoServer::run() {
    if (!is_running()) {
        auto start_res = start();
        if (!start_res) {
            return start_res;
        }
    }

    while (running_) {
        auto handle_res = accept_and_handle_one();
        if (!handle_res) {
            if (!running_) {
                break; // Server was stopped
            }
            // If accept encountered a transient error, check category
            if (handle_res.error().category == core::ErrorCategory::interrupted ||
                handle_res.error().category == core::ErrorCategory::would_block) {
                continue;
            }
            return handle_res;
        }
    }

    return core::Result<void>::success();
}

core::Result<void> EchoServer::accept_and_handle_one() {
    auto client_res = listener_.accept();
    if (!client_res) {
        if (!running_) {
            return core::Result<void>::success();
        }
        return client_res.error();
    }

    Socket client = std::move(client_res.value());

    // Milestone 02: Configure every accepted socket explicitly.
    auto nonblocking_res = client.set_nonblocking(true);
    if (!nonblocking_res) {
        client.close();
        return nonblocking_res.error();
    }

    // Run the reliable echo session with bounded buffer
    run_echo_session(client, config_);

    // Idempotent and clean close
    client.close();

    return core::Result<void>::success();
}

} // namespace net
