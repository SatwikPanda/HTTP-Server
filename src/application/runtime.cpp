#include "application/runtime.hpp"
#include "concurrency/event_loop/event_loop_driver.hpp"
#include "concurrency/thread_pool/thread_pool_driver.hpp"
#include "http/http_server.hpp"
#include "http/serializer.hpp"
#include "observability/metrics_registry.hpp"
#include "observability/process_sampler.hpp"
#include "proxy/cache_transaction.hpp"
#include "server/static_service.hpp"
#include <algorithm>
#include <atomic>
#include <sstream>
#include <thread>

namespace application {
struct Runtime::State {
    core::RuntimeConfig config;
    observability::MetricsRegistry metrics;
    mutable observability::ProcessSampler sampler;
    std::shared_ptr<proxy::ProxyStats> proxy_stats{std::make_shared<proxy::ProxyStats>()};
    std::shared_ptr<proxy::CacheStats> cache_stats{std::make_shared<proxy::CacheStats>()};
    std::shared_ptr<cache::storage::LruStorage> storage;
    std::shared_ptr<concurrency::helpers::HelperPool> helpers;
    std::unique_ptr<concurrency::thread_pool::ThreadPoolDriver> pool;
    std::unique_ptr<concurrency::event_loop::EventLoopDriver> loop;
    std::unique_ptr<http::HttpServer> single;
    std::thread single_thread;
    net::Endpoint endpoint;
    std::atomic<bool> stopped{false};
    void stop() noexcept {
        if (stopped.exchange(true)) return;
        if (pool) pool->request_stop(config.drain_timeout);
        if (loop) loop->request_stop(config.drain_timeout);
        if (single) single->stop();
    }
    void join() noexcept {
        if (pool) pool->join();
        if (loop) loop->join();
        if (single_thread.joinable()) single_thread.join();
        if (helpers) helpers->stop();
    }
    ~State() { stop(); join(); }
};

Runtime::Runtime(std::unique_ptr<State> state) : state_(std::move(state)) {}
Runtime::~Runtime() = default;
void Runtime::request_stop() noexcept { state_->stop(); }
void Runtime::join() noexcept { state_->join(); }
net::Endpoint Runtime::local_endpoint() const { return state_->endpoint; }

core::Result<std::unique_ptr<Runtime>> Runtime::start(core::RuntimeConfig config,
    std::shared_ptr<const http::TransactionFactory> custom_application) {
    if (custom_application && config.application != core::ApplicationMode::demo) {
        return core::make_error(core::ErrorCategory::invalid_argument, "custom application mode");
    }
    auto valid = core::validate_config(config);
    if (!valid) return valid.error();
    if (config.workers > 1024 || config.helpers > 256 || config.job_queue > 65536 ||
        config.completion_queue > 65536 || config.accept_queue > 65536 ||
        (config.driver == core::DriverMode::thread_pool &&
         (config.workers > config.sessions || config.completion_queue < config.workers))) {
        return core::make_error(core::ErrorCategory::invalid_argument, "application configuration", 0,
            "workers <= min(1024, max-sessions, completion-queue), helpers <= 256, queues <= 65536 required");
    }
    auto endpoint = net::Endpoint::from_string(config.bind_address, config.port);
    if (!endpoint) return core::make_error(core::ErrorCategory::invalid_argument, "Invalid endpoint", 0, endpoint.error().to_string());
    auto state = std::make_unique<State>();
    state->config = config;
    http::HttpSessionConfig session;
    session.metrics = &state->metrics;
    session.keep_alive = true;
    session.max_header_bytes = config.header_limit;
    session.max_input_bytes = config.buffers.input;
    session.max_output_bytes = config.buffers.output;
    session.max_requests_per_connection = config.max_requests;
    session.idle_timeout = config.idle_timeout;
    session.request_progress_timeout = config.progress_timeout;
    session.output_progress_timeout = config.progress_timeout;
    session.application = std::move(custom_application);
    if (config.application == core::ApplicationMode::static_files) {
        server::StaticConfig sc{config.document_root};
        sc.max_chunk_bytes = std::min<std::size_t>(65536, config.buffers.output);
        sc.work_timeout = config.work_timeout;
        auto factory = server::make_static_factory(sc);
        if (!factory) return factory.error();
        session.application = std::move(factory.value());
    } else if (config.application == core::ApplicationMode::proxy) {
        proxy::ProxyConfig pc;
        pc.limits.connect_timeout = config.connect_timeout;
        pc.limits.response_timeout = config.progress_timeout;
        pc.limits.body_timeout = config.progress_timeout;
        pc.limits.max_active_transactions = config.upstreams;
        pc.limits.response_limits.max_body_bytes = 16 * 1024 * 1024;
        if (config.cache.enabled) {
            cache::storage::Limits limits;
            limits.entries = config.cache.max_entries;
            limits.object_bytes = config.cache.object_bytes;
            limits.resident_bytes = config.cache.resident_bytes;
            limits.total_bytes = config.cache.total_bytes;
            limits.readers = config.sessions;
            auto storage = cache::storage::LruStorage::create(limits);
            if (!storage) return storage.error();
            state->storage = std::move(storage.value());
            auto factory = proxy::make_cache_factory(state->storage, {pc, {config.cache.object_bytes}},
                state->cache_stats, state->proxy_stats);
            if (!factory) return factory.error();
            session.application = std::move(factory.value());
        } else {
            auto factory = proxy::make_forward_factory(pc, state->proxy_stats);
            if (!factory) return factory.error();
            session.application = std::move(factory.value());
        }
    }
    auto session_valid = http::validate_http_session_config(session);
    if (!session_valid) return session_valid.error();
    if (config.driver == core::DriverMode::single) {
        state->single = std::make_unique<http::HttpServer>(http::HttpServerConfig{endpoint.value(), session});
        auto started = state->single->start();
        if (!started) return started.error();
        state->endpoint = state->single->local_endpoint();
        auto* server = state->single.get();
        state->single_thread = std::thread([server] { (void)server->run(); });
    } else {
        concurrency::helpers::HelperConfig hc;
        hc.threads = config.helpers;
        hc.queue_capacity = config.job_queue;
        auto helpers = concurrency::helpers::HelperPool::start(hc);
        if (!helpers) return helpers.error();
        state->helpers = std::move(helpers.value());
        concurrency::SessionFactory sessions = [session](core::ConnectionId id)
            -> core::Result<std::unique_ptr<concurrency::ConnectionSession>> {
            return std::unique_ptr<concurrency::ConnectionSession>(std::make_unique<http::HttpSession>(session, id));
        };
        if (config.driver == core::DriverMode::thread_pool) {
            auto socket = net::create_tcp_socket(endpoint.value().family());
            if (!socket) return socket.error();
            auto listener = std::move(socket.value());
            auto bound = listener.bind(endpoint.value());
            if (!bound) return bound.error();
            auto listening = listener.listen(128);
            if (!listening) return listening.error();
            concurrency::thread_pool::ThreadPoolConfig pc;
            pc.workers = config.workers;
            pc.accept_queue = config.accept_queue;
            pc.queue_timeout = config.idle_timeout;
            pc.completion_capacity = 1; // one outstanding job per worker
            http::Response overload(http::StatusCode::ServiceUnavailable);
            overload.set_body("503 Service Unavailable\r\n", "text/plain");
            overload.headers.set("Connection", "close");
            auto wire = http::serialize_response(overload);
            if (!wire) return wire.error();
            auto bytes = std::as_bytes(std::span(wire.value().data(), wire.value().size()));
            auto driver = concurrency::thread_pool::ThreadPoolDriver::create(pc,
                {std::move(listener), sessions, state->helpers,
                 std::make_shared<const concurrency::Bytes>(bytes.begin(), bytes.end())});
            if (!driver) return driver.error();
            state->pool = std::move(driver.value());
            auto started = state->pool->start();
            if (!started) return started.error();
            state->endpoint = state->pool->local_endpoint();
        } else {
            concurrency::event_loop::EventLoopConfig ec;
            ec.endpoint = endpoint.value();
            ec.metrics = &state->metrics;
            ec.limits.max_sessions = config.sessions;
            ec.limits.max_upstreams = config.upstreams;
            ec.limits.max_connecting = config.upstreams;
            ec.limits.max_pending_jobs = config.job_queue;
            ec.limits.max_completion_queue = config.completion_queue;
            auto driver = concurrency::event_loop::make_event_loop_driver(ec, sessions, nullptr, state->helpers);
            if (!driver) return driver.error();
            state->loop = std::move(driver.value());
            auto started = state->loop->start();
            if (!started) return started.error();
            state->endpoint = state->loop->local_endpoint();
        }
    }
    return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
}

std::string Runtime::statistics_json() const {
    const auto& s = *state_;
    const auto m = s.metrics.snapshot();
    const auto p = s.proxy_stats->snapshot();
    std::ostringstream out;
    out << "{\"schema_version\":1,\"scope\":\"process_lifetime\",\"wire_bytes_completed\":"
        << m.counter(observability::Counter::wire_bytes_completed)
        << ",\"responses_completed\":" << m.counter(observability::Counter::responses_completed);
    const auto process = s.sampler.sample();
    out << ",\"process_cpu_microseconds\":";
    if (process.total_cpu_time) out << process.total_cpu_time->count(); else out << "null";
    out << ",\"process_rss_bytes\":";
    if (process.resident_memory_bytes) out << *process.resident_memory_bytes; else out << "null";
    if (s.pool) {
        auto d = s.pool->stats();
        out << ",\"accepted\":" << d.accepted << ",\"active\":" << d.active
            << ",\"peak_active\":" << d.peak_active << ",\"queued_accepts\":" << d.queued
            << ",\"peak_queued_accepts\":" << d.peak_queued << ",\"rejected\":" << d.rejected_full;
    } else if (s.loop) {
        auto d = s.loop->stats();
        out << ",\"accepted\":" << d.connections_accepted << ",\"active\":" << d.active_sessions
            << ",\"peak_active\":" << d.peak_active_sessions << ",\"queued_completions\":" << d.queued_completions
            << ",\"peak_queued_completions\":" << d.peak_queued_completions
            << ",\"rejected\":" << d.connections_rejected;
    } else out << ",\"accepted\":null,\"active\":null,\"peak_active\":null";
    if (s.helpers) {
        auto h = s.helpers->stats();
        out << ",\"queued_jobs\":" << h.queued << ",\"running_jobs\":" << h.running
            << ",\"helper_rejected\":" << h.saturated;
    }
    // ProxyStats counts admitted transactions, including cache hits, not sockets.
    out << ",\"active_proxy_transactions\":" << p.active << ",\"peak_proxy_transactions\":" << p.peak_active;
    if (s.storage) {
        auto c = s.storage->statistics();
        out << ",\"cache_hits\":" << s.cache_stats->count(proxy::CacheEvent::hit)
            << ",\"cache_misses\":" << s.cache_stats->count(proxy::CacheEvent::miss)
            << ",\"cache_revalidations\":" << s.cache_stats->count(proxy::CacheEvent::validation_request)
            << ",\"cache_304\":" << s.cache_stats->count(proxy::CacheEvent::validation_304)
            << ",\"cache_entries\":" << c.entries << ",\"cache_resident_bytes\":" << c.resident_bytes
            << ",\"cache_total_bytes\":" << c.total_bytes << ",\"cache_evictions\":" << c.evictions
            << ",\"cache_candidate_bytes\":" << c.candidate_bytes << ",\"cache_retained_bytes\":" << c.retained_bytes
            << ",\"cache_readers\":" << c.readers;
    }
    out << '}';
    return out.str();
}
} // namespace application
