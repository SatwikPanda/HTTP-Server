#include "observability/bounded_logger.hpp"
#include "observability/metrics_registry.hpp"
#include "observability/process_sampler.hpp"
#include "observability/status_audit.hpp"
#include "http/http_session.hpp"
#include "http/response_parser.hpp"
#include "server/static_service.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
using namespace std::chrono_literals;

class TestChannel final : public net::StreamChannel {
public:
    explicit TestChannel(std::string input) : input_(std::move(input)) {}
    net::ReadResult read_some(std::span<std::byte> out) override {
        if (offset_ == input_.size()) return net::ReadResult::eof();
        const auto count = std::min(out.size(), input_.size() - offset_);
        std::memcpy(out.data(), input_.data() + offset_, count);
        offset_ += count;
        return net::ReadResult::success(count);
    }
    net::WriteResult write_some(std::span<const std::byte> in) override {
        output.append(reinterpret_cast<const char*>(in.data()), in.size());
        return net::WriteResult::success(in.size());
    }
    bool is_valid() const noexcept override { return valid_; }
    void close() noexcept override { valid_ = false; }
    std::string output;
private:
    std::string input_;
    std::size_t offset_{0};
    bool valid_{true};
};

void test_metrics_known_workload_and_histograms() {
    observability::MetricsRegistry registry;
    core::MemoryBudget budget(1024 * 1024);
    auto res_cand = test::take(budget.reserve(4096, core::AllocationKind::candidate));
    auto res_res = test::take(budget.reserve(8192, core::AllocationKind::resident));
    auto res_pin = test::take(budget.reserve(2048, core::AllocationKind::pinned));

    const auto temp_dir = std::filesystem::temp_directory_path() / "httpserver-obs-workload";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    CHECK(std::filesystem::create_directories(temp_dir, ec));
    const std::string payload = "Hello observability metrics!";
    {
        std::ofstream f(temp_dir / "index.html", std::ios::binary);
        f << payload;
    }

    server::StaticConfig static_cfg;
    static_cfg.document_root = temp_dir;
    auto factory = test::take(server::make_static_factory(static_cfg));

    http::HttpSessionConfig session_cfg;
    session_cfg.application = factory;
    session_cfg.keep_alive = true;
    session_cfg.metrics = &registry;

    const std::string pipeline =
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    TestChannel channel(pipeline);
    const auto stats = http::run_http_session(channel, session_cfg);
    CHECK(stats.completed_cleanly && stats.requests_served == 3);

    registry.record_status(200);
    registry.record_status(200);
    registry.record_status(404);
    registry.add_gauge(observability::Gauge::active_sessions, 3);
    registry.sub_gauge(observability::Gauge::active_sessions, 2);
    registry.observe_latency(observability::LatencyKind::request, 150us);
    registry.observe_latency(observability::LatencyKind::request, 900us);
    registry.observe_latency(observability::LatencyKind::request, 4500us);

    const auto snap = registry.snapshot(&budget);
    CHECK(snap.counter(observability::Counter::requests_started) == 3);
    CHECK(snap.counter(observability::Counter::responses_completed) == 3);
    CHECK(snap.counter(observability::Counter::wire_bytes_completed) == channel.output.size());
    // Body bytes exclude HEAD: 28 bytes from GET / plus 15 bytes from 404 Not Found\r\n.
    CHECK(snap.counter(observability::Counter::body_bytes_completed) == payload.size() + 15);
    CHECK(snap.counter(observability::Counter::status_200) == 2);
    CHECK(snap.counter(observability::Counter::status_404) == 1);
    CHECK(snap.gauge(observability::Gauge::active_sessions) == 1);
    CHECK(snap.peak_gauge(observability::Gauge::active_sessions) == 3);

    const auto& req_lat = snap.latency(observability::LatencyKind::request);
    CHECK(req_lat.total_count == 3);
    CHECK(req_lat.max_microseconds == 4500);
    CHECK(req_lat.percentile_upper_bound_us(0.50) == 1000);
    CHECK(req_lat.percentile_upper_bound_us(0.99) == 5000);

    CHECK(snap.memory.candidate_bytes == 4096);
    CHECK(snap.memory.resident_bytes == 8192);
    CHECK(snap.memory.pinned_bytes == 2048);
    CHECK(snap.memory.enforced_used_bytes == 4096 + 8192 + 2048);
    CHECK(snap.memory.enforced_capacity_bytes == 1024 * 1024);

    std::filesystem::remove_all(temp_dir, ec);
}

void test_bounded_logger_saturation_and_redaction() {
    std::vector<observability::LogRecord> collected;
    {
        // Manual drain mode (start_writer = false) to deterministically test ring overflow.
        observability::BoundedLogger logger(
            3,
            observability::LogOverflowPolicy::drop_newest,
            [&](const observability::LogRecord& rec) { collected.push_back(rec); },
            false
        );

        const core::OperationId id{{{7, 1}, 2}, core::EndpointRole::downstream, 4};
        CHECK(logger.log(observability::make_log_record(
            observability::LogLevel::info, id, "accept", "client accepted")));
        CHECK(logger.log(observability::make_log_record(
            observability::LogLevel::warning, id, "auth_check", "Authorization: Bearer secret-token")));
        CHECK(logger.log(observability::make_log_record(
            observability::LogLevel::error, id, "read", "connection reset",
            core::make_error(core::ErrorCategory::connection_reset, "read", 104, "peer reset"), 502)));
        // 4th record exceeds capacity=3 and must be dropped under drop_newest.
        CHECK(!logger.log(observability::make_log_record(
            observability::LogLevel::info, id, "overflow", "dropped record")));

        const auto st_before = logger.stats();
        CHECK(st_before.enqueued == 3 && st_before.dropped == 1 && st_before.queued == 3);

        CHECK(logger.drain_some(10) == 3);
        const auto st_after = logger.stats();
        CHECK(st_after.written == 3 && st_after.queued == 0);
    }

    CHECK(collected.size() == 3);
    CHECK(collected[0].operation_view() == "accept");
    // Sensitive Authorization header content must be redacted on admission.
    CHECK(collected[1].message_view() == "[redacted-sensitive]");
    const auto formatted = observability::format_log_record(collected[2]);
    CHECK(formatted.find("conn=7.1") != std::string::npos);
    CHECK(formatted.find("tx=2") != std::string::npos);
    CHECK(formatted.find("status=502") != std::string::npos);
    CHECK(formatted.find("native=104") != std::string::npos);

    // Background writer mode with drop_oldest policy.
    std::atomic<std::size_t> async_written{0};
    observability::BoundedLogger async_logger(
        8,
        observability::LogOverflowPolicy::drop_oldest,
        [&](const observability::LogRecord&) { async_written.fetch_add(1, std::memory_order_relaxed); },
        true
    );
    for (int i = 0; i < 16; ++i) {
        (void)async_logger.log(observability::make_log_record(
            observability::LogLevel::info, {{{1, 1}, 1}, core::EndpointRole::downstream, static_cast<std::uint64_t>(i)},
            "step", "ok"));
    }
    CHECK(async_logger.shutdown(500ms));
    const auto final_stats = async_logger.stats();
    CHECK(final_stats.written + final_stats.dropped == final_stats.enqueued);
}

void test_process_sampler_rate_limiting() {
    core::ManualSteadyClock clock(core::SteadyTimePoint{10s});
    observability::ProcessSampler sampler(100ms, clock);

    const auto s1 = sampler.sample();
    CHECK(s1.timestamp == core::SteadyTimePoint{10s});
    // On Windows/Linux/macOS, CPU time and resident memory are supported.
    CHECK(s1.total_cpu_time.has_value());
    CHECK(s1.resident_memory_bytes.has_value() && *s1.resident_memory_bytes > 0);

    clock.advance(50ms);
    const auto cached = sampler.sample();
    CHECK(cached.timestamp == s1.timestamp); // rate-limited within 100ms

    clock.advance(60ms);
    const auto s2 = sampler.sample();
    CHECK(s2.timestamp == core::SteadyTimePoint{10s} + 110ms);
    CHECK(s2.cpu_utilization_ratio.has_value());
    CHECK(*s2.cpu_utilization_ratio >= 0.0);
}

void test_status_contract_audit() {
    CHECK(observability::kAuditedStatusCodes.size() == 17);
    for (const auto code : observability::kAuditedStatusCodes) {
        CHECK(observability::is_audited_status(code));
        const auto get_pol = observability::status_policy_for(code, http::Method::GET);
        const auto head_pol = observability::status_policy_for(code, http::Method::HEAD);
        CHECK(!head_pol.body_allowed);
        if (code == http::StatusCode::NotModified) {
            CHECK(!get_pol.body_allowed);
        } else {
            CHECK(get_pol.body_allowed);
        }
        if (code == http::StatusCode::MethodNotAllowed) {
            CHECK(get_pol.requires_allow_header);
        }

        if (static_cast<int>(code) >= 400) {
            const auto resp_get = observability::build_sanitized_error_response(code, http::Method::GET, false);
            const auto resp_head = observability::build_sanitized_error_response(code, http::Method::HEAD, false);
            CHECK(!resp_get.body.empty());
            CHECK(resp_head.body.empty());
            CHECK(resp_get.headers.get("Content-Length") == resp_head.headers.get("Content-Length"));
            if (code == http::StatusCode::MethodNotAllowed) {
                CHECK(resp_get.headers.get("Allow") == std::optional<std::string_view>("GET, HEAD"));
            }
        }
    }
    CHECK(observability::can_emit_error_response(false));
    CHECK(!observability::can_emit_error_response(true));
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    test_metrics_known_workload_and_histograms();
    test_bounded_logger_saturation_and_redaction();
    test_process_sampler_rate_limiting();
    test_status_contract_audit();
    return 0;
}
