#include "test_support.hpp"
using namespace std::chrono_literals;

// This adapter must run the production session and driver, using injected I/O
// and a fake steady clock. Its contract is documented in tests/README.md.
#if defined(MILESTONE04_AVAILABLE)
#include "milestone04_adapter.hpp"
#include "core/clock.hpp"
#include <array>
#include <chrono>
#include <vector>

namespace {
template <typename ManualClock, typename TimePoint>
void check_manual_clock() {
    ManualClock clock;
    CHECK(clock.now() == TimePoint{});
    const auto start = TimePoint{10s};
    clock.set(start);
    CHECK(clock.now() == start);
    clock.advance(1500ms);
    CHECK(clock.now() == start + 1500ms);
    clock.advance(-250ms);
    CHECK(clock.now() == start + 1250ms);
    clock.advance(0ns);
    CHECK(clock.now() == start + 1250ms);

    ManualClock precise(start);
    // Wall-clock ticks differ across platforms. Conversion may lose less than
    // one native tick, but must never round a positive increment upward.
    constexpr auto increment = 123456789ns;
    precise.advance(increment);
    const auto elapsed = precise.now() - start;
    CHECK(elapsed <= increment);
    CHECK(increment - elapsed < typename TimePoint::duration{1});
    precise.advance(-increment);
    CHECK(precise.now() == start);
}
} // namespace

int main() {
    check_manual_clock<core::ManualSteadyClock, core::SteadyTimePoint>();
    check_manual_clock<core::ManualSystemClock, core::SystemTimePoint>();
    using tests::SessionHarness;
    std::vector<std::byte> payload(8193);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = std::byte(i % 256);
    SessionHarness session(/*input limit*/ 128, /*output limit*/ 128, 100ms);
    session.force_write_chunk(3);
    session.block_next_writes(5);
    session.receive(payload);
    session.peer_eof();
    session.run_until_idle();
    CHECK(session.output() == payload);
    CHECK(session.closed());
    CHECK(session.peak_input_bytes() <= 128);
    CHECK(session.peak_output_bytes() <= 128);
    for (bool pending_output : {false, true}) {
        SessionHarness timeout(128, 128, 100ms);
        if (pending_output) { timeout.block_next_writes(1000); timeout.receive(payload); }
        timeout.advance(99ms);
        CHECK(!timeout.closed());
        timeout.advance(2ms);
        CHECK(timeout.closed());
        CHECK(timeout.open_resources() == 0);
        SessionHarness stopping(128, 128, 100ms);
        if (pending_output) { stopping.block_next_writes(1000); stopping.receive(payload); }
        stopping.stop(10ms);
        stopping.advance(11ms);
        CHECK(stopping.closed());
        CHECK(stopping.open_resources() == 0);
    }
    CHECK(tests::echo_session_real_socket_roundtrip(payload));
    CHECK(tests::driver_accepts_two_sequential_clients());
}
#else
int main() { return test::pending(4); }
#endif
