#include "net/echo_server.hpp"
#include "net/runtime.hpp"

#include <csignal>
#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "Signal notification requires a lock-free atomic flag");
#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_stop_requested.store(true, std::memory_order_relaxed);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}
#else
void signal_handler(int) { g_stop_requested.store(true, std::memory_order_relaxed); }
#endif

// A signal handler cannot safely lock a mutex or use iostreams. A normal thread
// notices the flag and performs stop()/wake(), and joins before server teardown.
class SignalRegistration {
public:
    SignalRegistration() {
#ifdef _WIN32
        installed_ = SetConsoleCtrlHandler(console_ctrl_handler, TRUE) != FALSE;
#else
        previous_int_ = std::signal(SIGINT, signal_handler);
        previous_term_ = std::signal(SIGTERM, signal_handler);
        installed_ = previous_int_ != SIG_ERR && previous_term_ != SIG_ERR;
#endif
    }
    ~SignalRegistration() {
#ifdef _WIN32
        if (installed_) SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#else
        if (previous_int_ != SIG_ERR) std::signal(SIGINT, previous_int_);
        if (previous_term_ != SIG_ERR) std::signal(SIGTERM, previous_term_);
#endif
    }
    bool installed() const noexcept { return installed_; }
private:
    bool installed_{};
#ifndef _WIN32
    using Handler = void (*)(int);
    Handler previous_int_{}, previous_term_{};
#endif
};

template<class Integer>
bool parse_unsigned(std::string_view text, Integer& output) {
    if (text.empty()) return false;
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
    output = value;
    return true;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  --host <ip>          Bind address (default: 127.0.0.1)\n"
              << "  --port <port>        Port to listen on (default: 8080)\n"
              << "  --buffer-size <size> Echo buffer capacity in bytes (default: 4096)\n"
              << "  --help               Display this help message\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t buffer_size = 4096;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            if (!parse_unsigned(std::string_view(argv[++i]), port)) {
                std::cerr << "Invalid port: expected an integer from 0 to 65535\n";
                return 1;
            }
        } else if (arg == "--buffer-size" && i + 1 < argc) {
            if (!parse_unsigned(std::string_view(argv[++i]), buffer_size) ||
                buffer_size == 0 || buffer_size > 16 * 1024 * 1024) {
                std::cerr << "Invalid buffer size: expected an integer from 1 to 16777216\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // 1. Initialize Network Runtime
    auto runtime_res = net::initialize_network();
    if (!runtime_res) {
        std::cerr << "Failed to initialize network runtime: "
                  << runtime_res.error().to_string() << "\n";
        return 1;
    }

    // 2. Configure Endpoint
    auto ep_res = net::Endpoint::from_string(host, port);
    if (!ep_res) {
        std::cerr << "Invalid endpoint [" << host << ":" << port << "]: "
                  << ep_res.error().to_string() << "\n";
        return 1;
    }

    net::EchoConfig config;
    config.endpoint = ep_res.value();
    config.buffer_capacity = buffer_size;

    // 3. Initialize EchoServer
    net::EchoServer server(config);

    // 4. Start Server
    auto start_res = server.start();
    if (!start_res) {
        std::cerr << "Server startup failure: " << start_res.error().to_string() << "\n";
        return 1;
    }

    SignalRegistration signals;
    if (!signals.installed()) {
        std::cerr << "Could not install shutdown handlers\n";
        return 1;
    }
    std::jthread shutdown_monitor([&server](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (g_stop_requested.load(std::memory_order_relaxed)) { server.stop(); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    std::cout << "========================================\n"
              << " Readiness TCP Echo Server (Milestone 03)\n"
              << "========================================\n"
              << "Listening on       : " << server.local_endpoint().to_string() << "\n"
              << "Echo buffer limit  : " << config.buffer_capacity << " bytes\n"
              << "Status             : Ready for connections\n"
              << "Press Ctrl+C to stop.\n"
              << "----------------------------------------\n" << std::endl;

    auto run_res = server.run();
    if (!run_res) {
        std::cerr << "Server runtime error: " << run_res.error().to_string() << "\n";
        return 1;
    }

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}
