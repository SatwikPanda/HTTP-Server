#include "core/config.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
using namespace std::chrono_literals;

void test_defaults_and_help() {
    const core::RuntimeConfig defaults{};
    CHECK(core::validate_config(defaults));
    CHECK(defaults.application == core::ApplicationMode::demo);
    CHECK(defaults.driver == core::DriverMode::single);
    CHECK(defaults.bind_address == "127.0.0.1");
    CHECK(defaults.port == 8080);
    CHECK(!defaults.cache.enabled);

    const auto empty_parse = test::take(core::parse_cli(std::span<const std::string_view>{}, "httpserver"));
    CHECK(!empty_parse.help_requested);
    CHECK(empty_parse.config.bind_address == "127.0.0.1");
    CHECK(empty_parse.config.port == 8080);

    const std::array<std::string_view, 1> help_args{"--help"};
    const auto help_parse = test::take(core::parse_cli(help_args, "custom-bin"));
    CHECK(help_parse.help_requested);
    CHECK(help_parse.help_text.find("Usage: custom-bin") != std::string::npos);
    CHECK(help_parse.help_text.find("--mode <demo|server|proxy>") != std::string::npos);
    CHECK(help_parse.help_text.find("--concurrency <single|thread-pool|event-loop>") != std::string::npos);
    CHECK(help_parse.help_text.find("--cache <on|off>") != std::string::npos);
}

void test_valid_mode_combinations_and_migration_aliases() {
    {
        const std::array<std::string_view, 10> server_args{
            "--mode", "server",
            "--concurrency", "thread-pool",
            "--bind", "0.0.0.0",
            "--port", "9090",
            "--root", "www"
        };
        const auto parsed = test::take(core::parse_cli(server_args));
        CHECK(parsed.config.application == core::ApplicationMode::static_files);
        CHECK(parsed.config.driver == core::DriverMode::thread_pool);
        CHECK(parsed.config.bind_address == "0.0.0.0");
        CHECK(parsed.config.port == 9090);
        CHECK(parsed.config.document_root == std::filesystem::path("www"));
    }
    {
        const std::array<std::string_view, 14> proxy_args{
            "--mode", "proxy",
            "--concurrency", "event-loop",
            "--cache", "on",
            "--cache-entries", "512",
            "--cache-object-bytes", "65536",
            "--cache-resident-bytes", "1048576",
            "--cache-total-bytes", "2097152"
        };
        const auto parsed = test::take(core::parse_cli(proxy_args));
        CHECK(parsed.config.application == core::ApplicationMode::proxy);
        CHECK(parsed.config.driver == core::DriverMode::event_loop);
        CHECK(parsed.config.cache.enabled);
        CHECK(parsed.config.cache.max_entries == 512);
        CHECK(parsed.config.cache.object_bytes == 65536);
        CHECK(parsed.config.cache.resident_bytes == 1048576);
        CHECK(parsed.config.cache.total_bytes == 2097152);
    }
    {
        // Legacy milestone 03/06 spellings: --host and --buffer-size / --header-limit
        const std::array<std::string_view, 4> legacy_args{
            "--host", "127.0.0.1",
            "--buffer-size", "32768"
        };
        const auto parsed = test::take(core::parse_cli(legacy_args));
        CHECK(parsed.config.bind_address == "127.0.0.1");
        CHECK(parsed.config.header_limit == 32768);
        CHECK(parsed.config.buffers.input == 32768); // auto-widened when --input-bytes not set
    }
    {
        // Drain timeout may be 0 ms for immediate teardown.
        const std::array<std::string_view, 2> zero_drain{"--drain-timeout-ms", "0"};
        const auto parsed = test::take(core::parse_cli(zero_drain));
        CHECK(parsed.config.drain_timeout == 0ms);
    }
}

void test_invalid_and_conflicting_options() {
    auto expect_invalid = [](std::initializer_list<std::string_view> list) {
        std::vector<std::string_view> args(list);
        const auto res = core::parse_cli(args);
        CHECK(!res && res.error().category == core::ErrorCategory::invalid_argument);
    };

    // Port zero asks the OS for an ephemeral port, used by isolated runs.
    const std::array<std::string_view, 2> ephemeral{"--port", "0"};
    CHECK(test::take(core::parse_cli(ephemeral)).config.port == 0);
    // Numeric syntax: negative, plus sign, overflow, trailing junk
    expect_invalid({"--port", "-1"});
    expect_invalid({"--port", "+8080"});
    expect_invalid({"--port", "65536"});
    expect_invalid({"--port", "80junk"});
    expect_invalid({"--port", "abc"});
    expect_invalid({"--workers", "0"});
    expect_invalid({"--workers", "1000001"});
    expect_invalid({"--helpers", "0"});
    expect_invalid({"--max-sessions", "0"});
    expect_invalid({"--max-upstreams", "0"});
    expect_invalid({"--accept-queue", "0"});
    expect_invalid({"--job-queue", "0"});
    expect_invalid({"--completion-queue", "0"});
    expect_invalid({"--max-requests-per-connection", "0"});

    // Buffer relationships: low < high <= output, 4 <= header_limit <= input <= 16 MiB
    expect_invalid({"--buffer-size", "3"});
    expect_invalid({"--buffer-size", "16777217"});
    expect_invalid({"--header-limit", "8192", "--input-bytes", "4096"});
    expect_invalid({"--output-bytes", "127"});
    expect_invalid({"--low-water-bytes", "0"});
    expect_invalid({"--low-water-bytes", "49152", "--high-water-bytes", "49152"});
    expect_invalid({"--high-water-bytes", "70000", "--output-bytes", "65536"});

    // Cache hierarchy: object <= resident <= total, and proxy-only restriction
    expect_invalid({"--cache", "on"}); // default mode is demo
    expect_invalid({"--mode", "server", "--cache", "on"});
    expect_invalid({"--mode", "server", "--cache-entries", "128"});
    expect_invalid({"--mode", "proxy", "--cache-object-bytes", "0"});
    expect_invalid({"--mode", "proxy", "--cache-object-bytes", "2000000", "--cache-resident-bytes", "1000000"});
    expect_invalid({"--mode", "proxy", "--cache-resident-bytes", "40000000", "--cache-total-bytes", "33554432"});

    // Conflicting mode-specific options
    expect_invalid({"--mode", "demo", "--root", "www"});
    expect_invalid({"--mode", "proxy", "--root", "www"});

    // Timeouts: > 0 and <= 24h (except drain >= 0)
    expect_invalid({"--idle-timeout-ms", "0"});
    expect_invalid({"--progress-timeout-ms", "0"});
    expect_invalid({"--connect-timeout-ms", "0"});
    expect_invalid({"--work-timeout-ms", "0"});
    expect_invalid({"--idle-timeout-ms", "86400001"});
    expect_invalid({"--drain-timeout-ms", "86400001"});

    // Unknown or incomplete options
    expect_invalid({"--unknown-flag"});
    expect_invalid({"--mode"});
    expect_invalid({"--mode", "invalid"});
    expect_invalid({"--concurrency", "invalid"});
    expect_invalid({"--cache", "maybe"});
}

void test_document_root_validation() {
    const auto temp_base = std::filesystem::temp_directory_path() / "httpserver-cfg-root-test";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    CHECK(std::filesystem::create_directories(temp_base, ec));

    CHECK(core::validate_document_root(temp_base));
    CHECK(!core::validate_document_root(temp_base / "nonexistent"));

    const auto file_path = temp_base / "file.txt";
    {
        std::ofstream out(file_path, std::ios::binary);
        out << "hello";
    }
    CHECK(!core::validate_document_root(file_path));

    const auto symlink_dir = temp_base / "linked_dir";
    std::filesystem::create_directory_symlink(temp_base, symlink_dir, ec);
    if (!ec) {
        CHECK(!core::validate_document_root(symlink_dir));
    } else {
        std::cout << "Directory symlink creation unavailable on host: " << ec.message() << '\n';
    }

    std::filesystem::remove_all(temp_base, ec);
}

} // namespace

int main() {
    test_defaults_and_help();
    test_valid_mode_combinations_and_migration_aliases();
    test_invalid_and_conflicting_options();
    test_document_root_validation();
    return 0;
}
