#include "observability/stop_hooks.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>

namespace {
using namespace std::chrono_literals;

void test_controller_idempotence_and_partial_startup() {
    observability::StopController controller;
    CHECK(controller.phase() == observability::LifecyclePhase::constructed);
    CHECK(!controller.stop_requested());

    std::atomic<unsigned> callback_count{0};
    // Request stop during partial startup before callback is registered.
    controller.set_phase(observability::LifecyclePhase::starting);
    controller.request_stop();
    CHECK(controller.stop_requested());
    CHECK(controller.phase() == observability::LifecyclePhase::draining);

    // Registering callback after stop_requested() must invoke it immediately once.
    controller.set_stop_callback([&] { callback_count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(callback_count.load() == 1);

    // Repeated stop requests are idempotent.
    controller.request_stop();
    controller.request_stop();
    CHECK(callback_count.load() == 1);
    CHECK(controller.wait_for_stop(10ms));

    // Reset and test stop arriving while running.
    controller.reset();
    callback_count.store(0);
    controller.set_phase(observability::LifecyclePhase::running);
    controller.set_stop_callback([&] { callback_count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(callback_count.load() == 0);

    controller.request_stop();
    CHECK(callback_count.load() == 1);
    CHECK(controller.phase() == observability::LifecyclePhase::draining);
    controller.set_phase(observability::LifecyclePhase::stopped);
    controller.request_stop();
    CHECK(controller.phase() == observability::LifecyclePhase::stopped);
    CHECK(callback_count.load() == 1);
}

void test_scoped_os_stop_hooks_bridge() {
    observability::StopController controller;
    controller.set_phase(observability::LifecyclePhase::running);
    std::atomic<unsigned> woken{0};
    controller.set_stop_callback([&] { woken.fetch_add(1, std::memory_order_relaxed); });

    {
        observability::ScopedStopHooks hooks(controller, 5ms);
        CHECK(hooks.installed());
        observability::ScopedStopHooks::simulate_os_stop_signal();
        CHECK(controller.wait_for_stop(1s));
        CHECK(woken.load() == 1);
    }

    // After ScopedStopHooks destruction, simulating a signal must not touch controller.
    controller.reset();
    woken.store(0);
    controller.set_stop_callback([&] { woken.fetch_add(1, std::memory_order_relaxed); });
    observability::ScopedStopHooks::simulate_os_stop_signal();
    CHECK(!controller.wait_for_stop(25ms));
    CHECK(woken.load() == 0);
}

} // namespace

int main() {
    test_controller_idempotence_and_partial_startup();
    test_scoped_os_stop_hooks_bridge();
    return 0;
}
