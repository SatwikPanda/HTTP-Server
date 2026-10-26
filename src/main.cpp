#include "application/runtime.hpp"
#include "net/runtime.hpp"
#include "observability/stop_hooks.hpp"
#include <iostream>

int main(int argc, char *argv[])
{
    try
    {
        auto parsed = core::parse_cli(argc, argv);
        if (!parsed)
        {
            std::cerr << parsed.error().to_string() << '\n';
            return 1;
        }
        if (parsed.value().help_requested)
        {
            std::cout << parsed.value().help_text;
            return 0;
        }
        auto network = net::initialize_network();
        if (!network)
        {
            std::cerr << network.error().to_string() << '\n';
            return 1;
        }
        auto started = application::Runtime::start(parsed.value().config);
        if (!started)
        {
            std::cerr << started.error().to_string() << '\n';
            return 1;
        }
        auto app = std::move(started.value());
        observability::StopController stop;
        stop.set_stop_callback([&app]
                               { app->request_stop(); });
        observability::ScopedStopHooks hooks(stop);
        if (!hooks.installed())
        {
            std::cerr << "Could not install shutdown handlers\n";
            return 1;
        }
        stop.set_phase(observability::LifecyclePhase::running);
        std::cout << "Listening on : " << app->local_endpoint().to_string() << std::endl;
        while (!stop.wait_for_stop(std::chrono::seconds(1)))
        {
            std::cout << "METRICS " << app->statistics_json() << std::endl;
        }
        stop.set_phase(observability::LifecyclePhase::draining);
        app->join();
        stop.set_phase(observability::LifecyclePhase::stopped);
        std::cout << "METRICS " << app->statistics_json() << "\nServer shutdown complete.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Startup/runtime failure: " << error.what() << '\n';
        return 1;
    }
}
