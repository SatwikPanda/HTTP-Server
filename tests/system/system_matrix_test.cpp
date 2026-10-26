#include "system/test_inventory.hpp"
#include "fixtures/io_controls.hpp"
#include "fixtures/parallel_io.hpp"
#include "fixtures/system_runner.hpp"
#include "http/response_parser.hpp"

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

void verify_inventory_and_run_available_matrix() {
    CHECK(!test::kRuntimeSuiteInventory.empty());
    for (const auto& item : test::kRuntimeSuiteInventory) {
        CHECK(!item.target_name.empty());
        CHECK(!item.track.empty());
        CHECK(!item.milestone_label.empty());
        CHECK(item.implemented_in_current_tree);
    }

    std::size_t executed_combinations = 0;
    std::size_t pending_combinations = 0;
    for (const auto& entry : test::kProductMatrixInventory) {
        test::SystemScenarioSpec spec{};
        spec.application = entry.application;
        spec.driver = entry.driver;
        spec.cache_enabled = entry.cache_enabled;
        spec.name = std::string(entry.required_tracks);

        const auto avail = test::check_scenario_availability(spec);
        if (entry.ready_in_current_tree) {
            CHECK(avail == test::FeatureAvailability::available);
            run_static_server_scenario(spec);
            ++executed_combinations;
        } else {
            CHECK(avail != test::FeatureAvailability::available);
            run_static_server_scenario(spec); // logs explicit pending diagnostic
            ++pending_combinations;
        }
    }
    CHECK(executed_combinations == 1);
    CHECK(pending_combinations == 6);
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    verify_inventory_and_run_available_matrix();
    return 0;
}
