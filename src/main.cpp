#include "net/echo_server.hpp"
#include "net/runtime.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

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

net::EchoServer* g_server = nullptr;

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            if (g_server) {
                std::cout << "\nShutdown signal received. Stopping echo server..." << std::endl;
                g_server->stop();
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}
#else
void signal_handler(int sig) {
    if (g_server) {
        std::cout << "\nShutdown signal (" << sig << ") received. Stopping echo server..." << std::endl;
        g_server->stop();
    }
}
#endif

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
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--buffer-size" && i + 1 < argc) {
            buffer_size = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
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
    g_server = &server;

#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // 4. Start Server
    auto start_res = server.start();
    if (!start_res) {
        std::cerr << "Server startup failure: " << start_res.error().to_string() << "\n";
        return 1;
    }

    std::cout << "========================================\n"
              << " Reliable TCP Echo Server (Milestone 02)\n"
              << "========================================\n"
              << "Listening on       : " << server.local_endpoint().to_string() << "\n"
              << "Echo buffer limit  : " << config.buffer_capacity << " bytes\n"
              << "Status             : Ready for connections\n"
              << "Press Ctrl+C to stop.\n"
              << "----------------------------------------\n" << std::endl;

    auto run_res = server.run();
    if (!run_res && server.is_running()) {
        std::cerr << "Server runtime error: " << run_res.error().to_string() << "\n";
        return 1;
    }

    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}