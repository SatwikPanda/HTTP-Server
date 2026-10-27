#include "routes.hpp"

#include "observability/stop_hooks.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    try {
        std::uint32_t port = 8080;
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: http_app [port]\nDefault port: 8080. Use 0 to choose an available port.\n";
            return 0;
        }
        if (argc > 2) {
            std::cerr << "Usage: http_app [port]\n";
            return 1;
        }
        if (argc == 2) {
            const std::string_view text = argv[1];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port > 65535) {
                std::cerr << "Port must be an integer from 0 to 65535\n";
                return 1;
            }
        }

        application::Server server;
        auto routes = app::register_routes(server);
        if (!routes) {
            std::cerr << routes.error().to_string() << '\n';
            return 1;
        }
        auto listening = server.listen(port);
        if (!listening) {
            std::cerr << listening.error().to_string() << '\n';
            return 1;
        }

        observability::StopController stop;
        stop.set_stop_callback([&server] { server.request_stop(); });
        observability::ScopedStopHooks hooks(stop);
        if (!hooks.installed()) {
            std::cerr << "Could not install shutdown handlers\n";
            return 1;
        }
        std::cout << "Listening on : " << listening.value().to_string() << std::endl;
        server.wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Application failure: " << error.what() << '\n';
        return 1;
    }
}
