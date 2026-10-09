#include "net/echo_server.hpp"
#include "net/stream.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

class ControlledStreamChannel : public net::StreamChannel {
public:
    std::vector<std::byte> input_data;
    std::size_t input_read_offset{0};
    std::size_t max_read_chunk{0}; // 0 = unlimited
    int read_would_block_cycles{0};

    std::vector<std::byte> output_data;
    std::size_t max_write_chunk{0}; // 0 = unlimited, > 0 forces short writes
    int write_would_block_cycles{0};
    int write_would_block_reload{0}; // reload cycles every write

    bool is_closed{false};

    std::size_t total_short_writes{0};
    std::size_t total_write_calls{0};
    std::size_t total_read_calls{0};

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        total_read_calls++;
        if (is_closed) {
            return net::ReadResult::failure(
                core::make_error(core::ErrorCategory::bad_descriptor, "read_some")
            );
        }
        if (read_would_block_cycles > 0) {
            read_would_block_cycles--;
            return net::ReadResult::would_block_result();
        }
        if (input_read_offset >= input_data.size()) {
            return net::ReadResult::eof();
        }

        std::size_t available = input_data.size() - input_read_offset;
        std::size_t to_copy = std::min(available, destination.size());
        if (max_read_chunk > 0) {
            to_copy = std::min(to_copy, max_read_chunk);
        }

        std::memcpy(destination.data(), input_data.data() + input_read_offset, to_copy);
        input_read_offset += to_copy;
        return net::ReadResult::success(to_copy);
    }

    [[nodiscard]] net::WriteResult write_some(std::span<const std::byte> source) override {
        total_write_calls++;
        if (is_closed) {
            return net::WriteResult::failure(
                core::make_error(core::ErrorCategory::bad_descriptor, "write_some")
            );
        }
        if (write_would_block_cycles > 0) {
            write_would_block_cycles--;
            return net::WriteResult::would_block_result();
        }

        std::size_t to_write = source.size();
        if (max_write_chunk > 0 && to_write > max_write_chunk) {
            to_write = max_write_chunk;
            total_short_writes++;
        }

        const auto* src = reinterpret_cast<const std::byte*>(source.data());
        output_data.insert(output_data.end(), src, src + to_write);

        if (write_would_block_reload > 0) {
            write_would_block_cycles = write_would_block_reload;
        }

        return net::WriteResult::success(to_write);
    }

    void close() noexcept override {
        is_closed = true;
    }

    [[nodiscard]] bool is_valid() const noexcept override {
        return !is_closed;
    }
};

} // namespace

void test_empty_disconnect() {
    std::cout << "[RUN] test_empty_disconnect..." << std::endl;
    ControlledStreamChannel channel;
    // input_data is empty -> immediate EOF on read

    net::EchoConfig config;
    config.buffer_capacity = 64;

    auto stats = net::run_echo_session(channel, config);
    assert(stats.completed_cleanly);
    assert(stats.total_bytes_read == 0);
    assert(stats.total_bytes_written == 0);
    assert(channel.output_data.empty());

    std::cout << "[PASS] test_empty_disconnect" << std::endl;
}

void test_forced_short_writes_and_would_block() {
    std::cout << "[RUN] test_forced_short_writes_and_would_block..." << std::endl;
    ControlledStreamChannel channel;

    // Create 100 bytes of sequential data
    for (int i = 0; i < 100; ++i) {
        channel.input_data.push_back(static_cast<std::byte>(i));
    }

    // Force short writes: max 7 bytes per write call
    channel.max_write_chunk = 7;
    // Force initial would-block: 2 would-block returns before write succeeds
    channel.write_would_block_cycles = 2;

    net::EchoConfig config;
    config.buffer_capacity = 32;
    config.wait_retry_delay = std::chrono::milliseconds(1);

    auto stats = net::run_echo_session(channel, config);

    assert(stats.completed_cleanly);
    assert(stats.total_bytes_read == 100);
    assert(stats.total_bytes_written == 100);
    assert(channel.total_short_writes > 0);
    assert(stats.short_write_count > 0);
    assert(stats.would_block_write_count == 2);
    assert(channel.output_data == channel.input_data);

    std::cout << "[PASS] test_forced_short_writes_and_would_block (short writes: "
              << stats.short_write_count << ", would_blocks: " << stats.would_block_write_count << ")" << std::endl;
}

void test_binary_bytes_all_values() {
    std::cout << "[RUN] test_binary_bytes_all_values..." << std::endl;
    ControlledStreamChannel channel;

    // Cover all 256 possible byte values including 0x00 and 0xFF
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int b = 0; b < 256; ++b) {
            channel.input_data.push_back(static_cast<std::byte>(b));
        }
    }
    assert(channel.input_data.size() == 1024);

    net::EchoConfig config;
    config.buffer_capacity = 128;

    auto stats = net::run_echo_session(channel, config);
    assert(stats.completed_cleanly);
    assert(stats.total_bytes_read == 1024);
    assert(stats.total_bytes_written == 1024);
    assert(channel.output_data == channel.input_data);

    std::cout << "[PASS] test_binary_bytes_all_values" << std::endl;
}

void test_fragmented_sends() {
    std::cout << "[RUN] test_fragmented_sends..." << std::endl;
    ControlledStreamChannel channel;

    for (int i = 0; i < 200; ++i) {
        channel.input_data.push_back(static_cast<std::byte>(i & 0xFF));
    }

    // Force reads to arrive 1 byte at a time
    channel.max_read_chunk = 1;

    net::EchoConfig config;
    config.buffer_capacity = 64;

    auto stats = net::run_echo_session(channel, config);
    assert(stats.completed_cleanly);
    assert(stats.total_bytes_read == 200);
    assert(stats.total_bytes_written == 200);
    assert(channel.output_data == channel.input_data);

    std::cout << "[PASS] test_fragmented_sends" << std::endl;
}

void test_payload_larger_than_buffer() {
    std::cout << "[RUN] test_payload_larger_than_buffer..." << std::endl;
    ControlledStreamChannel channel;

    // 20,000 bytes payload
    for (std::size_t i = 0; i < 20000; ++i) {
        channel.input_data.push_back(static_cast<std::byte>(i % 251));
    }

    // Tiny 16-byte buffer capacity!
    net::EchoConfig config;
    config.buffer_capacity = 16;
    channel.max_write_chunk = 5; // force short writes within each chunk

    auto stats = net::run_echo_session(channel, config);
    assert(stats.completed_cleanly);
    assert(stats.total_bytes_read == 20000);
    assert(stats.total_bytes_written == 20000);
    assert(channel.output_data == channel.input_data);

    std::cout << "[PASS] test_payload_larger_than_buffer (16-byte buffer echoed 20,000 bytes)" << std::endl;
}

void test_slow_receiver_simulation() {
    std::cout << "[RUN] test_slow_receiver_simulation..." << std::endl;
    ControlledStreamChannel channel;

    for (int i = 0; i < 500; ++i) {
        channel.input_data.push_back(static_cast<std::byte>(i % 127));
    }

    // Slow receiver: accepts only 3 bytes at a time and would_blocks on each step
    channel.max_write_chunk = 3;
    channel.write_would_block_reload = 1; // causes a would-block before each successful short write

    net::EchoConfig config;
    config.buffer_capacity = 64;
    config.wait_retry_delay = std::chrono::milliseconds(0); // instant retry in test

    auto stats = net::run_echo_session(channel, config);
    assert(stats.completed_cleanly);
    assert(stats.total_bytes_read == 500);
    assert(stats.total_bytes_written == 500);
    assert(stats.would_block_write_count > 0);
    assert(stats.short_write_count > 0);
    assert(channel.output_data == channel.input_data);

    std::cout << "[PASS] test_slow_receiver_simulation (would_blocks: "
              << stats.would_block_write_count << ")" << std::endl;
}

int main() {
    test_empty_disconnect();
    test_forced_short_writes_and_would_block();
    test_binary_bytes_all_values();
    test_fragmented_sends();
    test_payload_larger_than_buffer();
    test_slow_receiver_simulation();

    std::cout << "\nALL ECHO TEST DOUBLE TESTS PASSED!" << std::endl;
    return 0;
}
