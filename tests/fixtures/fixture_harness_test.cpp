#include "fixtures/io_controls.hpp"
#include "fixtures/parallel_io.hpp"
#include "fixtures/system_runner.hpp"
#include "http/http_session.hpp"
#include "http/reference_application.hpp"
#include "http/response_parser.hpp"
#include "server/static_service.hpp"

namespace {
using namespace std::chrono_literals;

void test_scripted_origin_multi_request_and_barriers() {
    test::ScriptedOrigin origin({
        test::OriginResponseScript::fixed_length(
            200, "OK", "first-body", {{"ETag", "W/\"v1\""}}, 5, "req1_gate"),
        test::OriginResponseScript::fixed_length(
            304, "Not Modified", "", {{"ETag", "W/\"v1\""}}, 64, {}),
        test::OriginResponseScript::raw(
            {"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"}, 16, {},
            test::ScriptTerminalBehavior::truncate_after_first_segment)
    });

    http::HttpSessionConfig cfg;
    cfg.max_output_bytes = 512;
    cfg.application = http::make_origin_reference_factory(origin.target(), "/item?k=1");

    // Transaction 1: blocked on named barrier "req1_gate", released by test thread.
    std::string out1;
    std::thread client1([&] {
        test::ControlledByteChannel ch("GET /item?k=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        const auto st = http::run_http_session(ch, cfg);
        CHECK(st.completed_cleanly && st.requests_served == 1);
        out1 = ch.output();
    });

    origin.wait_for_barrier("req1_gate");
    CHECK(origin.request_count() == 1);
    origin.release_barrier("req1_gate");
    client1.join();

    const auto frame1 = test::inspect_fixed_wire_response(out1);
    CHECK(frame1.valid && frame1.status_code == 200);
    CHECK(frame1.body == "first-body");
    CHECK(test::fnv1a_64(frame1.body) == test::fnv1a_64("first-body"));

    // Transaction 2: 304 Not Modified (no body).
    {
        test::ControlledByteChannel ch("GET /item?k=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        const auto st = http::run_http_session(ch, cfg);
        CHECK(st.completed_cleanly && st.requests_served == 1);
        const auto frame2 = test::inspect_fixed_wire_response(ch.output(), true);
        CHECK(frame2.valid && frame2.status_code == 304 && frame2.body.empty());
    }

    // Transaction 3: truncated origin body after head committed -> downstream closes without 2nd response.
    {
        test::ControlledByteChannel ch("GET /item?k=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        const auto st = http::run_http_session(ch, cfg);
        CHECK(!st.completed_cleanly && st.error.is_error());
        CHECK(ch.output().starts_with("HTTP/1.1 200 OK\r\n"));
    }

    origin.stop();
    origin.join();
    CHECK(origin.accepted_connections() == 3);
    const auto captured = origin.captured_requests();
    CHECK(captured.size() == 3);
    CHECK(captured[0].parse_ok && captured[0].method == http::Method::GET && captured[0].target == "/item?k=1");
}

void test_byte_channel_and_synchronized_clocks() {
    test::TemporaryDirectory dir;
    const std::string payload = "0123456789abcdef0123456789abcdef";
    (void)dir.write("nested/data.txt", payload);

    server::StaticConfig scfg;
    scfg.document_root = dir.path();
    auto factory = test::take(server::make_static_factory(scfg));

    http::HttpSessionConfig cfg;
    cfg.application = factory;
    cfg.keep_alive = true;
    cfg.max_output_bytes = 256;

    const std::string req =
        "GET /nested/data.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD /nested/data.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    test::ControlledByteChannel channel(req, 5, 7);
    channel.set_read_would_block_every(2);
    channel.set_write_would_block_every(3);

    const auto stats = http::run_http_session(channel, cfg);
    CHECK(stats.completed_cleanly && stats.requests_served == 2);
    CHECK(stats.would_block_read_count > 0 && stats.would_block_write_count > 0);

    // Verify with independent wire oracle as well as production parser.
    const auto first_frame = test::inspect_fixed_wire_response(channel.output(), false);
    CHECK(first_frame.valid && first_frame.status_code == 200);
    CHECK(first_frame.body == payload);
    CHECK(first_frame.has_header_line("Connection: keep-alive"));

    const auto second_wire = std::string_view(channel.output()).substr(first_frame.total_wire_bytes);
    const auto second_frame = test::inspect_fixed_wire_response(second_wire, true);
    CHECK(second_frame.valid && second_frame.status_code == 200 && second_frame.body.empty());
    CHECK(second_frame.has_header_line("Connection: close"));

    test::SynchronizedManualSteadyClock steady_clock(core::SteadyTimePoint{1s});
    test::SynchronizedManualSystemClock sys_clock(core::SystemTimePoint{100s});
    steady_clock.advance(250ms);
    sys_clock.advance(5s);
    CHECK(steady_clock.now() == core::SteadyTimePoint{1s} + 250ms);
    CHECK(sys_clock.now() == core::SystemTimePoint{105s});
}

void test_ipv6_loopback_fixture_coverage() {
    auto sock_res = net::create_tcp_socket(net::AddressFamily::ipv6);
    if (!sock_res) {
        std::cout << "IPv6 socket creation unavailable on host: " << sock_res.error().to_string() << '\n';
        return;
    }
    auto listen_sock = std::move(sock_res.value());
    if (!listen_sock.bind(net::Endpoint::ipv6_loopback(0)) || !listen_sock.listen()) {
        std::cout << "IPv6 loopback bind/listen unavailable on host\n";
        return;
    }
    CHECK(listen_sock.set_nonblocking(true));
    const auto ep = test::take(listen_sock.local_endpoint());
    CHECK(ep.family() == net::AddressFamily::ipv6 && ep.port() != 0);
    listen_sock.close();
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    test_scripted_origin_multi_request_and_barriers();
    test_byte_channel_and_synchronized_clocks();
    test_ipv6_loopback_fixture_coverage();
    return 0;
}
