#include "resource_probe.hpp"
#include "fixtures/io_controls.hpp"
#include "fixtures/parallel_io.hpp"
#include "fixtures/system_runner.hpp"
#include "http/date.hpp"
#include "observability/process_sampler.hpp"
#include "test_support.hpp"

#include <array>
#include <charconv>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;

std::size_t metric(std::string_view json, std::string_view name) {
    const auto key = '"' + std::string(name) + "\":";
    const auto position = json.find(key);
    CHECK(position != std::string_view::npos);
    const auto tail = json.substr(position + key.size());
    std::size_t value = 0;
    CHECK(std::from_chars(tail.data(), tail.data() + tail.size(), value).ec == std::errc{});
    return value;
}

void check_response(std::string_view wire, int status, std::string_view body, bool head = false) {
    const auto response = test::inspect_fixed_wire_response(wire, head);
    if (!response.valid || response.status_code != status) {
        std::cerr << "Expected " << status << ", received " << response.status_code
            << ", valid=" << response.valid << ", wire=" << wire.substr(0, 512) << '\n';
    }
    CHECK(response.valid && response.status_code == status);
    CHECK(response.body == body && response.total_wire_bytes == wire.size());
}

void burst(const std::function<void(int)>& action) {
    std::vector<std::jthread> clients;
    for (int client = 0; client < 4; ++client) clients.emplace_back([&, client] { action(client); });
    for (auto& client : clients) client.join();
}

void warm_thread_runtime() {
    // Process-level wait primitives may be initialized only under contention.
    // Exercise them above the application's peak before recording a baseline;
    // no application socket/file/cache owner is kept alive by this warmup.
    std::mutex mutex;
    std::condition_variable cv;
    int ready = 0;
    bool release = false;
    std::vector<std::jthread> threads;
    for (int i = 0; i < 32; ++i) threads.emplace_back([&] {
        std::unique_lock lock(mutex);
        ++ready; cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, 5s, [&] { return ready == 32; }));
        release = true;
    }
    cv.notify_all();
    for (auto& thread : threads) thread.join();
}

void quiesce(test::LoopbackSystemHarness& app, bool cached) {
    app.stop(); app.stop(); app.join(); app.join();
    const auto stats = app.statistics_json();
    CHECK(metric(stats, "active") == 0);
    CHECK(metric(stats, "active_proxy_transactions") == 0);
    CHECK(metric(stats, "queued_jobs") == 0 && metric(stats, "running_jobs") == 0);
    if (cached) {
        CHECK(metric(stats, "cache_readers") == 0);
        CHECK(metric(stats, "cache_candidate_bytes") == 0);
        CHECK(metric(stats, "cache_retained_bytes") == 0);
        CHECK(metric(stats, "cache_entries") <= 2);
        CHECK(metric(stats, "cache_total_bytes") <= core::CacheConfig{}.total_bytes);
    }
    std::cout << "QUIESCENT " << stats << '\n';
}

void static_cycle(core::DriverMode driver) {
    test::TemporaryDirectory root;
    const std::string body(131072, 's');
    (void)root.write("index.html", body);
    test::SystemScenarioSpec spec;
    spec.driver = driver; spec.document_root = root.path();
    test::LoopbackSystemHarness app(spec);
    burst([&](int client) {
        for (int round = 0; round < 6; ++round) {
            const bool head = (client + round) % 2 != 0;
            const auto request = std::string(head ? "HEAD" : "GET") +
                " / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            check_response(app.send_and_receive(request), 200, head ? std::string_view{} : body, head);
        }
    });
    auto bad = test::inspect_fixed_wire_response(app.send_and_receive(
        "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"));
    CHECK(bad.valid && bad.status_code == 400);
    quiesce(app, false);
}

void proxy_cycle(core::DriverMode driver, bool cached) {
    using Script = test::OriginResponseScript;
    const auto date = http::format_http_date(core::real_system_clock().now());
    const std::string old_body(32768, 'a'), new_body(32768, 'b');
    const std::initializer_list<std::pair<std::string_view, std::string_view>> old_fields{
        {"Date", date}, {"Cache-Control", "max-age=3600"}, {"ETag", "\"a\""}};
    std::vector<Script> scripts;
    if (cached) {
        scripts.push_back(Script::fixed_length(200, "OK", old_body, old_fields));
        scripts.push_back(Script::fixed_length(200, "OK", new_body,
            {{"Date", date}, {"Cache-Control", "max-age=3600"}, {"ETag", "\"b\""}}, 4096, "replace"));
        // More keys than the two-entry cache force eviction and republishing.
        for (int i = 0; i < 4; ++i) scripts.push_back(Script::fixed_length(200, "OK", new_body, old_fields));
    } else {
        for (int i = 0; i < 24; ++i) scripts.push_back(Script::fixed_length(200, "OK", old_body));
    }
    test::ScriptedOrigin origin(std::move(scripts));
    test::SystemScenarioSpec spec;
    spec.application = core::ApplicationMode::proxy; spec.driver = driver; spec.cache_enabled = cached;
    spec.cache_entries = 2;
    test::LoopbackSystemHarness app(spec);
    auto request = [&](std::string_view path, std::string_view fields = {}) {
        return "GET http://" + origin.endpoint().to_string() + std::string(path) +
            " HTTP/1.1\r\nHost: ignored.example\r\n" + std::string(fields) + "Connection: close\r\n\r\n";
    };
    if (cached) {
        check_response(app.send_and_receive(request("/object")), 200, old_body);
        std::jthread replacement([&] {
            check_response(app.send_and_receive(request("/object", "Cache-Control: no-cache\r\n")), 200, new_body);
        });
        origin.wait_for_barrier("replace");
        burst([&](int) {
            for (int i = 0; i < 6; ++i) {
                const auto wire = app.send_and_receive(request("/object", "Cache-Control: only-if-cached\r\n"));
                check_response(wire, 200, old_body);
                CHECK(wire.find("ETag: \"a\"\r\n") != std::string::npos);
            }
        });
        origin.release_barrier("replace"); replacement.join();
        const auto wire = app.send_and_receive(request("/object", "Cache-Control: only-if-cached\r\n"));
        check_response(wire, 200, new_body);
        CHECK(wire.find("ETag: \"b\"\r\n") != std::string::npos && origin.request_count() == 2);
        for (int i = 0; i < 4; ++i) check_response(app.send_and_receive(request("/key" + std::to_string(i))), 200, new_body);
        CHECK(metric(app.statistics_json(), "cache_evictions") >= 3);
        const auto absent = test::inspect_fixed_wire_response(app.send_and_receive(request("/object", "Cache-Control: only-if-cached\r\n")));
        CHECK(absent.valid && absent.status_code == 504 && origin.request_count() == 6);
    } else {
        burst([&](int) {
            for (int i = 0; i < 6; ++i) check_response(app.send_and_receive(request("/object")), 200, old_body);
        });
        CHECK(origin.request_count() == 24);
    }
    quiesce(app, cached);
    origin.stop(); origin.join();
}

void print_sample(int cycle, const test::ResourceSample& sample, observability::ProcessSampler& memory) {
    const auto rss = memory.force_sample().resident_memory_bytes;
    auto value = [](auto item) { return item ? std::to_string(*item) : "null"; };
    std::cout << "RESOURCE {\"cycle\":" << cycle << ",\"handles\":" << value(sample.handles)
        << ",\"threads\":" << value(sample.threads) << ",\"rss_bytes\":" << value(rss) << "}" << std::endl;
    for (const auto& [type, count] : sample.handle_types) std::cout << "HANDLE_TYPE " << type << '=' << count << '\n';
}
}

int main(int argc, char** argv) {
    int cycles = 6;
    if (argc == 2) {
        const std::string_view text(argv[1]);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), cycles);
        CHECK(result.ec == std::errc{} && result.ptr == text.data() + text.size() && cycles >= 1 && cycles <= 1000);
    } else CHECK(argc == 1);
    auto network = test::take(net::initialize_network());
    observability::ProcessSampler memory;
    warm_thread_runtime();
    (void)test::resource_sample(); // Initialize optional native sampling itself.
    auto run_cycle = [] {
        for (const auto driver : {core::DriverMode::thread_pool, core::DriverMode::event_loop}) {
            static_cycle(driver); proxy_cycle(driver, false); proxy_cycle(driver, true);
        }
    };
    run_cycle(); // Warm OS/STL lazy initialization before the ownership baseline.
    const auto baseline = test::resource_sample();
    print_sample(0, baseline, memory);
    CHECK(baseline.handles && baseline.threads);
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        run_cycle();
        auto sample = test::resource_sample();
        const auto deadline = test::Clock::now() + 2s;
        // OS thread/handle bookkeeping can retire just after owned threads join.
        // Wait for the same baseline; never mask a sustained increase with slack.
        while (((sample.threads && *sample.threads > *baseline.threads) ||
                (sample.handles && *sample.handles > *baseline.handles)) && test::Clock::now() < deadline) {
            std::this_thread::sleep_for(1ms); sample = test::resource_sample();
        }
        print_sample(cycle, sample, memory);
        CHECK(sample.handles && *sample.handles <= *baseline.handles);
        CHECK(sample.threads && *sample.threads <= *baseline.threads);
    }
}
