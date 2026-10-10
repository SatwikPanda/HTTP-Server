#include "test_support.hpp"
using namespace std::chrono_literals;
#include "net/echo_server.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace {
class AdversarialChannel final : public net::StreamChannel {
public:
    enum class Mode { normal, blocked_read, blocked_write, zero_write, read_error, write_error };
    Mode mode{Mode::normal};
    std::vector<std::byte> input, output;
    std::size_t offset{}, largest_read{}, largest_write{}, calls{};
    bool valid{true};
    net::ReadResult read_some(std::span<std::byte> destination) override {
        ++calls;
        largest_read = std::max(largest_read, destination.size());
        if (mode == Mode::blocked_read) return net::ReadResult::would_block_result();
        if (mode == Mode::read_error) return net::ReadResult::failure(
            core::make_error(core::ErrorCategory::connection_reset, "fake read"));
        if (offset == input.size()) return net::ReadResult::eof();
        auto count = std::min({destination.size(), input.size() - offset, std::size_t{11}});
        std::copy_n(input.data() + offset, count, destination.data());
        offset += count;
        return net::ReadResult::success(count);
    }
    net::WriteResult write_some(std::span<const std::byte> source) override {
        ++calls;
        largest_write = std::max(largest_write, source.size());
        if (mode == Mode::blocked_write) return net::WriteResult::would_block_result();
        if (mode == Mode::zero_write) return net::WriteResult::success(0);
        if (mode == Mode::write_error) return net::WriteResult::failure(
            core::make_error(core::ErrorCategory::connection_reset, "fake write"));
        if (calls % 4 == 0) return net::WriteResult::would_block_result();
        auto count = std::min(source.size(), std::size_t{3});
        const auto prefix = source.first(count);
        output.insert(output.end(), prefix.begin(), prefix.end());
        return net::WriteResult::success(count);
    }
    void close() noexcept override { valid = false; }
    bool is_valid() const noexcept override { return valid; }
};
}

int main() {
    auto runtime = test::take(net::initialize_network());
    auto pair = test::pair();
    std::array<std::byte, 16> bytes{};
    CHECK(pair.server.read_some(bytes).would_block());
    CHECK(pair.server.read_some({}).is_ok());
    CHECK(pair.client.write_some({}).is_ok());
    CHECK(pair.client.shutdown_write());
    auto deadline = test::Clock::now() + 2s;
    for (;;) {
        auto read = pair.server.read_some(bytes);
        if (read.is_eof()) break;
        CHECK(read.would_block());
        CHECK(test::Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    pair.client.close();
    CHECK(pair.client.write_some(bytes).is_error());
    CHECK(pair.client.read_some(bytes).is_error());
    net::EchoConfig config;
    config.buffer_capacity = 17;
    config.wait_retry_delay = 0ms;
    config.retry_timeout = 20ms;
    AdversarialChannel channel;
    for (std::size_t i = 0; i < 20000; ++i) channel.input.push_back(std::byte(i % 256));
    auto stats = net::run_echo_session(channel, config);
    CHECK(stats.completed_cleanly);
    CHECK(channel.output == channel.input);
    CHECK(channel.largest_read <= config.buffer_capacity);
    CHECK(channel.largest_write <= config.buffer_capacity);
    CHECK(stats.short_write_count > 0);
    CHECK(stats.would_block_write_count > 0);
    config.wait_retry_delay = 1ms;
    for (auto mode : {AdversarialChannel::Mode::blocked_read,
                     AdversarialChannel::Mode::blocked_write,
                     AdversarialChannel::Mode::zero_write,
                     AdversarialChannel::Mode::read_error,
                     AdversarialChannel::Mode::write_error}) {
        AdversarialChannel failing;
        failing.mode = mode;
        failing.input = {std::byte{1}};
        auto start = test::Clock::now();
        auto result = net::run_echo_session(failing, config);
        CHECK(!result.completed_cleanly);
        CHECK(test::Clock::now() - start < 1s);
        CHECK(failing.calls < 1000); // zero progress must not reset the timeout
    }
}
