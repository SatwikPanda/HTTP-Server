#include "system/test_inventory.hpp"
#include "fixtures/io_controls.hpp"
#include "fixtures/parallel_io.hpp"
#include "fixtures/system_runner.hpp"
#include "http/response_parser.hpp"
#include "http/date.hpp"
#include <thread>

namespace {
using namespace std::chrono_literals;

// Shared parameterized scenario assertion running against any available SystemScenarioSpec.
void run_static_server_scenario(const test::SystemScenarioSpec& spec) {
    const auto avail = test::check_scenario_availability(spec);
    if (avail != test::FeatureAvailability::available) {
        std::cout << "[PENDING] scenario=" << spec.name << ": " << test::to_string(avail) << '\n';
        return;
    }

    test::TemporaryDirectory root;
    const std::string index_body = "<!doctype html><title>System Matrix</title>";
    const std::string binary_body(16384, '\x5a');
    const auto index_path = root.write("index.html", index_body);
    (void)root.write("sub/blob.bin", binary_body);
    root.set_mtime(index_path, std::chrono::system_clock::from_time_t(1700000000));

    auto active_spec = spec;
    active_spec.document_root = root.path();

    test::LoopbackSystemHarness harness(active_spec);

    // 1. Pipelined GET /, HEAD /sub/blob.bin, traversal rejection (403), and conditional 304.
    const std::string pipeline =
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD /sub/blob.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /../secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET / HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: *\r\nConnection: close\r\n\r\n";

    const auto raw_out = harness.send_and_receive(pipeline);

    // Verify first response with the independent wire oracle.
    const auto first_oracle = test::inspect_fixed_wire_response(raw_out, false);
    CHECK(first_oracle.valid && first_oracle.status_code == 200);
    CHECK(first_oracle.body == index_body);
    CHECK(test::fnv1a_64(first_oracle.body) == test::fnv1a_64(index_body));

    // Verify full 4-response pipeline ordering.
    const std::array<http::Method, 4> methods{
        http::Method::GET, http::Method::HEAD, http::Method::GET, http::Method::GET
    };
    std::size_t offset = 0;
    std::vector<http::Response> parsed;
    for (const auto m : methods) {
        http::ResponseParser parser(m);
        const auto res = parser.consume(std::string_view(raw_out).substr(offset));
        CHECK(res.event == http::ParseEvent::complete);
        offset += res.consumed;
        parsed.push_back(parser.response());
    }
    CHECK(offset == raw_out.size());
    CHECK(parsed[0].status_code == 200 && parsed[0].body == index_body);
    CHECK(parsed[1].status_code == 200 && parsed[1].body.empty());
    CHECK(parsed[1].headers.get("Content-Length") == std::optional<std::string_view>(std::to_string(binary_body.size())));
    CHECK(parsed[2].status_code == 403);
    CHECK(parsed[3].status_code == 304 && parsed[3].body.empty());

    harness.stop();
    harness.join();
    const auto diag = harness.diagnostics();
    CHECK(diag.accepted_connections == 1);
    CHECK(diag.active_connections == 0);
    CHECK(diag.last_transition == "stopped");
}

std::string proxy_request(const test::ScriptedOrigin& origin, std::string_view path,
                          std::string_view fields = {}, std::string_view method = "GET") {
    return std::string(method) + " http://" + origin.endpoint().to_string() + std::string(path) +
        " HTTP/1.1\r\nHost: ignored.example\r\n" + std::string(fields) + "Connection: close\r\n\r\n";
}

void expect_wire(std::string_view wire, int status, std::string_view body, bool head = false) {
    const auto parsed = test::inspect_fixed_wire_response(wire, head);
    CHECK(parsed.valid);
    CHECK(parsed.status_code == status);
    if (parsed.body != body) std::cerr << "expected=" << body << " actual=" << parsed.body.substr(0, 128)
        << " head=" << parsed.raw_header_block.substr(0, 512) << std::endl;
    CHECK(parsed.body == body);
    CHECK(parsed.total_wire_bytes == wire.size());
}

void run_proxy_scenario(const test::SystemScenarioSpec& spec) {
    using Script = test::OriginResponseScript;
    test::LoopbackSystemHarness harness(spec);
    test::ScriptedOrigin origin({
        Script::fixed_length(200, "OK", "fixed", {{"Cache-Control", "no-store"}, {"X-Origin", "one"}}),
        Script::raw({"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"}, 1),
        Script::raw({"HTTP/1.1 200 OK\r\nCache-Control: no-store\r\n\r\nclosed"}),
        Script::raw({"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\nCache-Control: no-store\r\n\r\nfinal"}),
        Script::raw({"HTTP/1.1 200 OK\r\nContent-Length: 123\r\nCache-Control: no-store\r\n\r\n"}),
        Script::raw({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"}),
        Script::raw({"HTTP/1.1 200 OK\r\nContent-Length: 99\r\nCache-Control: max-age=60\r\n\r\nshort"})
    });
    auto fixed = harness.send_and_receive(proxy_request(origin, "/fixed?x=1", "X-Keep: yes\r\nProxy-Authorization: secret\r\n"));
    expect_wire(fixed, 200, "fixed");
    CHECK(fixed.find("X-Origin: one\r\n") != std::string::npos);
    expect_wire(harness.send_and_receive(proxy_request(origin, "/chunked")), 200, "abcde");
    expect_wire(harness.send_and_receive(proxy_request(origin, "/close")), 200, "closed");
    expect_wire(harness.send_and_receive(proxy_request(origin, "/interim")), 200, "final");
    auto head = harness.send_and_receive(proxy_request(origin, "/head", {}, "HEAD"));
    expect_wire(head, 200, {}, true);
    CHECK(head.find("Content-Length: 123\r\n") != std::string::npos);
    const auto bad = test::inspect_fixed_wire_response(harness.send_and_receive(proxy_request(origin, "/bad")));
    CHECK(bad.valid && bad.status_code == 502);
    const auto truncated = harness.send_and_receive(proxy_request(origin, "/truncated"));
    CHECK(!test::inspect_fixed_wire_response(truncated).valid);
    origin.wait_for_requests(7);
    const auto requests = origin.captured_requests();
    CHECK(requests[0].parse_ok && requests[0].target == "/fixed?x=1");
    CHECK(requests[0].headers.get("Host") == std::optional<std::string_view>(origin.endpoint().to_string()));
    CHECK(requests[0].headers.get("X-Keep") == std::optional<std::string_view>("yes"));
    CHECK(!requests[0].headers.get("Proxy-Authorization"));
    CHECK(requests[0].headers.get("Via").has_value());
    if (spec.cache_enabled) {
        const auto missing = test::inspect_fixed_wire_response(harness.send_and_receive(
            proxy_request(origin, "/truncated", "Cache-Control: only-if-cached\r\n")));
        CHECK(missing.valid && missing.status_code == 504);
        CHECK(origin.request_count() == 7); // Partial bodies never become hits.
    }
    harness.stop(); harness.join();
    CHECK(harness.statistics_json().find("\"active\":0") != std::string::npos);
}

void run_cache_scenario(const test::SystemScenarioSpec& spec) {
    using Script = test::OriginResponseScript;
    const auto date = http::format_http_date(core::real_system_clock().now());
    test::LoopbackSystemHarness harness(spec);
    test::ScriptedOrigin origin({
        Script::fixed_length(200, "OK", "version-one", {{"Date", date}, {"Cache-Control", "max-age=0"}, {"ETag", "\"v1\""}}),
        Script::raw({"HTTP/1.1 304 Not Modified\r\nDate: " + date + "\r\nCache-Control: max-age=3600\r\nETag: \"v1\"\r\n\r\n"}),
        Script::fixed_length(200, "OK", "version-two", {{"Date", date}, {"Cache-Control", "max-age=3600"}, {"ETag", "\"v2\""}}, 97, "replacement")
    });
    auto miss = test::inspect_fixed_wire_response(harness.send_and_receive(
        proxy_request(origin, "/object", "Cache-Control: only-if-cached\r\n")));
    CHECK(miss.valid && miss.status_code == 504 && origin.request_count() == 0);
    expect_wire(harness.send_and_receive(proxy_request(origin, "/object")), 200, "version-one");
    expect_wire(harness.send_and_receive(proxy_request(origin, "/object")), 200, "version-one");
    origin.wait_for_requests(2);
    CHECK(origin.captured_requests()[1].headers.get("If-None-Match") == std::optional<std::string_view>("\"v1\""));
    auto hit = harness.send_and_receive(proxy_request(origin, "/object", "Cache-Control: only-if-cached\r\n"));
    expect_wire(hit, 200, "version-one");
    CHECK(hit.find("Age: ") != std::string::npos && origin.request_count() == 2);
    // Concurrent readers all observe the same published metadata/body pair.
    std::array<std::string, 8> results;
    std::vector<std::jthread> readers;
    for (std::size_t i = 0; i < results.size(); ++i) readers.emplace_back([&, i] {
        results[i] = harness.send_and_receive(proxy_request(origin, "/object", "Cache-Control: only-if-cached\r\n"));
    });
    for (auto& reader : readers) reader.join();
    for (const auto& result : results) expect_wire(result, 200, "version-one");
    CHECK(origin.request_count() == 2);
    std::string replaced;
    std::jthread replacing([&] {
        replaced = harness.send_and_receive(proxy_request(origin, "/object", "Cache-Control: no-cache\r\n"));
    });
    origin.wait_for_barrier("replacement");
    // A delayed validation must not block unrelated hits or expose new metadata
    // paired with the old body before the replacement commits.
    expect_wire(harness.send_and_receive(proxy_request(origin, "/object", "Cache-Control: only-if-cached\r\n")), 200, "version-one");
    origin.release_barrier("replacement");
    replacing.join();
    expect_wire(replaced, 200, "version-two");
    expect_wire(harness.send_and_receive(proxy_request(origin, "/object", "Cache-Control: only-if-cached\r\n")), 200, "version-two");
    CHECK(origin.request_count() == 3);
    harness.stop(); harness.join();
    CHECK(harness.statistics_json().find("\"active_proxy_transactions\":0") != std::string::npos);
}

void run_blocked_origin_shutdown(const test::SystemScenarioSpec& spec) {
    test::ScriptedOrigin origin({test::OriginResponseScript::fixed_length(200, "OK", "held", {}, 1, "held")});
    test::LoopbackSystemHarness harness(spec);
    auto client = test::take(net::create_tcp_socket());
    CHECK(client.connect(harness.endpoint()));
    const auto request = proxy_request(origin, "/held");
    const auto sent = client.write_some(std::as_bytes(std::span(request)));
    CHECK(sent.is_ok() && sent.bytes_transferred == request.size());
    origin.wait_for_barrier("held");
    client.close();
    harness.stop(); harness.join();
    origin.stop(); origin.join();
    const auto stats = harness.statistics_json();
    CHECK(stats.find("\"active\":0") != std::string::npos);
    CHECK(stats.find("\"active_proxy_transactions\":0") != std::string::npos);
}

void verify_inventory_and_run_available_matrix() {
    CHECK(!test::kRuntimeSuiteInventory.empty());
    for (const auto& item : test::kRuntimeSuiteInventory) {
        CHECK(!item.target_name.empty());
        CHECK(!item.track.empty());
        CHECK(!item.milestone_label.empty());
        CHECK(item.implemented_in_current_tree);
    }

    std::size_t executed_combinations = 0;
    for (const auto& entry : test::kProductMatrixInventory) {
        test::SystemScenarioSpec spec{};
        spec.application = entry.application;
        spec.driver = entry.driver;
        spec.cache_enabled = entry.cache_enabled;
        spec.name = std::string(entry.required_tracks);

        const auto avail = test::check_scenario_availability(spec);
        CHECK(entry.ready_in_current_tree && avail == test::FeatureAvailability::available);
        std::cout << "[RUN] " << entry.required_tracks << std::endl;
        if (spec.application == core::ApplicationMode::static_files) run_static_server_scenario(spec);
        else {
            run_proxy_scenario(spec);
            if (spec.cache_enabled) run_cache_scenario(spec);
            run_blocked_origin_shutdown(spec);
        }
        ++executed_combinations;
    }
    CHECK(executed_combinations == 7);
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    verify_inventory_and_run_available_matrix();
    return 0;
}
