#include "core/config.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <vector>

namespace core {
namespace {

template <class Integer>
bool parse_unsigned_strict(std::string_view text, Integer& output) noexcept {
    if (text.empty() || text.front() == '-' || text.front() == '+') {
        return false;
    }
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return false;
    }
    output = value;
    return true;
}

bool parse_duration_ms(std::string_view text, std::chrono::milliseconds& output) noexcept {
    std::uint64_t ms{};
    if (!parse_unsigned_strict(text, ms)) {
        return false;
    }
    constexpr auto max_ms = static_cast<std::uint64_t>(std::chrono::milliseconds(std::chrono::hours(24)).count());
    if (ms > max_ms) {
        return false;
    }
    output = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(ms));
    return true;
}

Error cli_error(std::string message) {
    return make_error(ErrorCategory::invalid_argument, "cli", 0, std::move(message));
}

} // namespace

Result<void> validate_config(const RuntimeConfig& c) {
    const auto positive = [](auto n) { return n > 0 && n <= 1000000; };
    const auto duration = [](auto d) { return d > std::chrono::milliseconds::zero() && d <= std::chrono::hours(24); };
    if (c.bind_address.empty() || c.bind_address.size() > 255 || c.bind_address.find('\0') != std::string::npos || c.port == 0 ||
        !positive(c.workers) || !positive(c.helpers) || !positive(c.sessions) || !positive(c.upstreams) ||
        !positive(c.accept_queue) || !positive(c.job_queue) || !positive(c.completion_queue) || !positive(c.max_requests) ||
        c.header_limit < 4 || c.header_limit > 16 * 1024 * 1024 ||
        c.buffers.input < 4 || c.buffers.input > 16 * 1024 * 1024 ||
        c.header_limit > c.buffers.input ||
        c.buffers.output < 128 || c.buffers.output > 16 * 1024 * 1024 ||
        c.buffers.low_water == 0 || c.buffers.low_water >= c.buffers.high_water || c.buffers.high_water > c.buffers.output ||
        !positive(c.cache.max_entries) || c.cache.object_bytes == 0 || c.cache.object_bytes > c.cache.resident_bytes ||
        c.cache.resident_bytes > c.cache.total_bytes || (c.cache.enabled && c.application != ApplicationMode::proxy) ||
        !duration(c.idle_timeout) || !duration(c.progress_timeout) || !duration(c.connect_timeout) || !duration(c.work_timeout) ||
        c.drain_timeout < std::chrono::milliseconds::zero() || c.drain_timeout > std::chrono::hours(24) ||
        c.document_root.empty() || c.document_root.native().find('\0') != std::filesystem::path::string_type::npos) {
        return make_error(ErrorCategory::invalid_argument, "runtime configuration");
    }
    return {};
}

Result<void> validate_document_root(const std::filesystem::path& root) {
    if (root.empty()) {
        return make_error(ErrorCategory::invalid_argument, "document root", 0, "Document root is empty");
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(root, ec);
    if (ec) {
        return make_error(ErrorCategory::invalid_argument, "document root", ec.value(), ec.message());
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        return make_error(ErrorCategory::invalid_argument, "document root", 0, "Document root must be a non-symlink directory");
    }
    return {};
}

std::string format_help(std::string_view program_name) {
    const RuntimeConfig defaults{};
    std::ostringstream out;
    out << "Usage: " << (program_name.empty() ? "httpserver" : program_name) << " [options]\n"
        << "Options:\n"
        << "  --mode <demo|server|proxy>               Application mode (default: demo)\n"
        << "  --concurrency <single|thread-pool|event-loop>\n"
        << "                                           Driver concurrency mode (default: single)\n"
        << "  --bind <ip>, --host <ip>                 Bind address (default: " << defaults.bind_address << ")\n"
        << "  --port <port>                            Port to listen on (default: " << defaults.port << ")\n"
        << "  --root <path>                            Document root for server mode (default: .)\n"
        << "  --workers <count>                        Worker thread count (default: " << defaults.workers << ")\n"
        << "  --helpers <count>                        Helper thread count (default: " << defaults.helpers << ")\n"
        << "  --max-sessions <count>                   Max active client sessions (default: " << defaults.sessions << ")\n"
        << "  --max-upstreams <count>                  Max active upstream connections (default: " << defaults.upstreams << ")\n"
        << "  --accept-queue <count>                   Max queued accepted connections (default: " << defaults.accept_queue << ")\n"
        << "  --job-queue <count>                      Max queued helper jobs (default: " << defaults.job_queue << ")\n"
        << "  --completion-queue <count>               Max queued helper completions (default: " << defaults.completion_queue << ")\n"
        << "  --input-bytes <bytes>                    Per-connection input buffer limit (default: " << defaults.buffers.input << ")\n"
        << "  --output-bytes <bytes>                   Per-connection output buffer limit (default: " << defaults.buffers.output << ")\n"
        << "  --low-water-bytes <bytes>                Output low-water mark (default: " << defaults.buffers.low_water << ")\n"
        << "  --high-water-bytes <bytes>               Output high-water mark (default: " << defaults.buffers.high_water << ")\n"
        << "  --header-limit <bytes>, --buffer-size <bytes>\n"
        << "                                           Request header byte limit (default: " << defaults.header_limit << ")\n"
        << "  --max-requests-per-connection <count>    Keep-alive request limit (default: " << defaults.max_requests << ")\n"
        << "  --idle-timeout-ms <ms>                   Idle connection timeout in ms (default: " << defaults.idle_timeout.count() << ")\n"
        << "  --progress-timeout-ms <ms>               Request/output progress timeout in ms (default: " << defaults.progress_timeout.count() << ")\n"
        << "  --connect-timeout-ms <ms>                Upstream connect timeout in ms (default: " << defaults.connect_timeout.count() << ")\n"
        << "  --work-timeout-ms <ms>                   Helper work timeout in ms (default: " << defaults.work_timeout.count() << ")\n"
        << "  --drain-timeout-ms <ms>                  Graceful shutdown drain timeout in ms (default: " << defaults.drain_timeout.count() << ")\n"
        << "  --cache <on|off>                         Enable shared proxy cache (default: off)\n"
        << "  --cache-entries <count>                  Max cached entries (default: " << defaults.cache.max_entries << ")\n"
        << "  --cache-object-bytes <bytes>             Max cacheable object bytes (default: " << defaults.cache.object_bytes << ")\n"
        << "  --cache-resident-bytes <bytes>           Max resident cache bytes (default: " << defaults.cache.resident_bytes << ")\n"
        << "  --cache-total-bytes <bytes>              Max total cache/in-flight budget bytes (default: " << defaults.cache.total_bytes << ")\n"
        << "  --help, -h                               Display this help message\n";
    return out.str();
}

Result<CliParseOutcome> parse_cli(std::span<const std::string_view> args, std::string_view program_name) {
    CliParseOutcome outcome{};
    RuntimeConfig& cfg = outcome.config;

    bool root_explicit = false;
    bool cache_explicit = false;
    bool cache_tuning_explicit = false;
    bool input_explicit = false;
    bool header_explicit = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            outcome.help_requested = true;
            outcome.help_text = format_help(program_name);
            return outcome;
        }

        const auto next_val = [&](std::string_view& val) -> bool {
            if (i + 1 >= args.size()) {
                return false;
            }
            val = args[++i];
            return true;
        };

        std::string_view val{};
        if (arg == "--mode") {
            if (!next_val(val)) return cli_error("Missing value for --mode");
            if (val == "demo") cfg.application = ApplicationMode::demo;
            else if (val == "server") cfg.application = ApplicationMode::static_files;
            else if (val == "proxy") cfg.application = ApplicationMode::proxy;
            else return cli_error("Invalid --mode: expected demo, server, or proxy");
        } else if (arg == "--concurrency") {
            if (!next_val(val)) return cli_error("Missing value for --concurrency");
            if (val == "single") cfg.driver = DriverMode::single;
            else if (val == "thread-pool") cfg.driver = DriverMode::thread_pool;
            else if (val == "event-loop") cfg.driver = DriverMode::event_loop;
            else return cli_error("Invalid --concurrency: expected single, thread-pool, or event-loop");
        } else if (arg == "--bind" || arg == "--host") {
            if (!next_val(val) || val.empty() || val.find('\0') != std::string_view::npos) {
                return cli_error("Invalid bind address");
            }
            cfg.bind_address = std::string(val);
        } else if (arg == "--port") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.port) || cfg.port == 0) {
                return cli_error("Invalid port: expected an integer from 1 to 65535");
            }
        } else if (arg == "--root") {
            if (!next_val(val) || val.empty() || val.find('\0') != std::string_view::npos) {
                return cli_error("Invalid --root path");
            }
            cfg.document_root = std::filesystem::path(val);
            root_explicit = true;
        } else if (arg == "--workers") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.workers)) {
                return cli_error("Invalid --workers");
            }
        } else if (arg == "--helpers") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.helpers)) {
                return cli_error("Invalid --helpers");
            }
        } else if (arg == "--max-sessions") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.sessions)) {
                return cli_error("Invalid --max-sessions");
            }
        } else if (arg == "--max-upstreams") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.upstreams)) {
                return cli_error("Invalid --max-upstreams");
            }
        } else if (arg == "--accept-queue") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.accept_queue)) {
                return cli_error("Invalid --accept-queue");
            }
        } else if (arg == "--job-queue") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.job_queue)) {
                return cli_error("Invalid --job-queue");
            }
        } else if (arg == "--completion-queue") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.completion_queue)) {
                return cli_error("Invalid --completion-queue");
            }
        } else if (arg == "--input-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.buffers.input)) {
                return cli_error("Invalid --input-bytes");
            }
            input_explicit = true;
        } else if (arg == "--output-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.buffers.output)) {
                return cli_error("Invalid --output-bytes");
            }
        } else if (arg == "--low-water-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.buffers.low_water)) {
                return cli_error("Invalid --low-water-bytes");
            }
        } else if (arg == "--high-water-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.buffers.high_water)) {
                return cli_error("Invalid --high-water-bytes");
            }
        } else if (arg == "--header-limit" || arg == "--buffer-size") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.header_limit) ||
                cfg.header_limit < 4 || cfg.header_limit > 16 * 1024 * 1024) {
                return cli_error("Invalid buffer size: expected an integer from 4 to 16777216");
            }
            header_explicit = true;
        } else if (arg == "--max-requests-per-connection") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.max_requests)) {
                return cli_error("Invalid --max-requests-per-connection");
            }
        } else if (arg == "--idle-timeout-ms") {
            if (!next_val(val) || !parse_duration_ms(val, cfg.idle_timeout)) {
                return cli_error("Invalid --idle-timeout-ms");
            }
        } else if (arg == "--progress-timeout-ms") {
            if (!next_val(val) || !parse_duration_ms(val, cfg.progress_timeout)) {
                return cli_error("Invalid --progress-timeout-ms");
            }
        } else if (arg == "--connect-timeout-ms") {
            if (!next_val(val) || !parse_duration_ms(val, cfg.connect_timeout)) {
                return cli_error("Invalid --connect-timeout-ms");
            }
        } else if (arg == "--work-timeout-ms") {
            if (!next_val(val) || !parse_duration_ms(val, cfg.work_timeout)) {
                return cli_error("Invalid --work-timeout-ms");
            }
        } else if (arg == "--drain-timeout-ms") {
            if (!next_val(val) || !parse_duration_ms(val, cfg.drain_timeout)) {
                return cli_error("Invalid --drain-timeout-ms");
            }
        } else if (arg == "--cache") {
            if (!next_val(val)) return cli_error("Missing value for --cache");
            if (val == "on") cfg.cache.enabled = true;
            else if (val == "off") cfg.cache.enabled = false;
            else return cli_error("Invalid --cache: expected on or off");
            cache_explicit = true;
        } else if (arg == "--cache-entries") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.cache.max_entries)) {
                return cli_error("Invalid --cache-entries");
            }
            cache_tuning_explicit = true;
        } else if (arg == "--cache-object-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.cache.object_bytes)) {
                return cli_error("Invalid --cache-object-bytes");
            }
            cache_tuning_explicit = true;
        } else if (arg == "--cache-resident-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.cache.resident_bytes)) {
                return cli_error("Invalid --cache-resident-bytes");
            }
            cache_tuning_explicit = true;
        } else if (arg == "--cache-total-bytes") {
            if (!next_val(val) || !parse_unsigned_strict(val, cfg.cache.total_bytes)) {
                return cli_error("Invalid --cache-total-bytes");
            }
            cache_tuning_explicit = true;
        } else {
            return cli_error("Unknown or incomplete argument: " + std::string(arg));
        }
    }

    if (header_explicit && !input_explicit && cfg.header_limit > cfg.buffers.input) {
        cfg.buffers.input = cfg.header_limit;
    }
    if (!header_explicit && input_explicit && cfg.buffers.input < cfg.header_limit) {
        cfg.header_limit = cfg.buffers.input;
    }

    if (root_explicit && cfg.application != ApplicationMode::static_files) {
        return cli_error("--root is only valid with --mode server");
    }
    if ((cache_explicit && cfg.cache.enabled && cfg.application != ApplicationMode::proxy) ||
        (cache_tuning_explicit && cfg.application != ApplicationMode::proxy)) {
        return cli_error("Cache options are only valid with --mode proxy");
    }

    auto valid = validate_config(cfg);
    if (!valid) {
        return valid.error();
    }
    return outcome;
}

Result<CliParseOutcome> parse_cli(int argc, const char* const argv[]) {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return cli_error("Invalid argument vector");
    }
    std::string_view prog = (argc > 0 && argv[0] != nullptr) ? std::string_view(argv[0]) : "httpserver";
    std::vector<std::string_view> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            if (argv[i] == nullptr) {
                return cli_error("Null argument string");
            }
            args.emplace_back(argv[i]);
        }
    }
    return parse_cli(args, prog);
}

} // namespace core
