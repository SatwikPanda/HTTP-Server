#include "test_support.hpp"
using namespace std::chrono_literals;

// This adapter must run the production session and driver, using injected I/O
// and a fake steady clock. Its contract is documented in tests/README.md.
#if defined(MILESTONE04_AVAILABLE)
#include "milestone04_adapter.hpp"
#include <array>
#include <vector>
int main() {
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
