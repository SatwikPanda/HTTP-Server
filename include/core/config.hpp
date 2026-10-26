#pragma once

#include "core/result.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace core {
enum class ApplicationMode { demo, static_files, proxy };
enum class DriverMode { single, thread_pool, event_loop };
struct BufferConfig { std::size_t input{16384}, output{65536}, low_water{16384}, high_water{49152}; };
struct CacheConfig { bool enabled{}; std::size_t max_entries{1024}, object_bytes{1048576}, resident_bytes{16777216}, total_bytes{33554432}; };
struct RuntimeConfig {
    ApplicationMode application{ApplicationMode::demo};
    DriverMode driver{DriverMode::single};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8080};
    std::filesystem::path document_root{"."};
    std::size_t workers{4}, helpers{2}, sessions{1024}, upstreams{256};
    std::size_t accept_queue{128}, job_queue{256}, completion_queue{256}, max_requests{100};
    std::size_t header_limit{8192};
    BufferConfig buffers;
    CacheConfig cache;
    std::chrono::milliseconds idle_timeout{5000}, progress_timeout{5000}, connect_timeout{5000}, work_timeout{5000}, drain_timeout{2000};
};

struct CliParseOutcome {
    RuntimeConfig config{};
    bool help_requested{false};
    std::string help_text{};
};

[[nodiscard]] Result<void> validate_config(const RuntimeConfig& config);
[[nodiscard]] Result<void> validate_document_root(const std::filesystem::path& root);
[[nodiscard]] std::string format_help(std::string_view program_name = "httpserver");
[[nodiscard]] Result<CliParseOutcome> parse_cli(std::span<const std::string_view> args, std::string_view program_name = "httpserver");
[[nodiscard]] Result<CliParseOutcome> parse_cli(int argc, const char* const argv[]);
} // namespace core
