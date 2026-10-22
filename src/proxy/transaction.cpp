#include "proxy/transaction.hpp"

#include "http/framing.hpp"
#include "http/response_parser.hpp"
#include "http/serializer.hpp"

#include <algorithm>
#include <optional>

namespace proxy {
namespace {
using namespace concurrency;
using http::StatusCode;

core::Error proxy_error(core::ErrorCategory category, std::string operation) {
    return core::make_error(category, std::move(operation));
}

Outcome outcome_for(StatusCode status) noexcept {
    const auto code = static_cast<int>(status);
    if (status == StatusCode::GatewayTimeout) return Outcome::gateway_timeout;
    if (status == StatusCode::ServiceUnavailable) return Outcome::unavailable;
    if (status == StatusCode::BadGateway) return Outcome::bad_gateway;
    if (code >= 400 && code < 500) return Outcome::client_error;
    return Outcome::internal_error;
}

class ForwardTransaction final : public http::Transaction {
public:
    ForwardTransaction(const http::Request& client, const http::TransactionContext& context,
                       const ProxyConfig& config, std::shared_ptr<ProxyStats> stats,
                       std::unique_ptr<ExchangeObserver> observer)
        : config_(config), stats_(std::move(stats)), observer_(std::move(observer)),
          steady_(context.steady_clock), wall_(context.wall_clock), method_(client.method),
          parser_(client.method, config.limits.response_limits) {
        if (!stats_ || !stats_->try_admit(config_.limits.max_active_transactions)) {
            rejection_ = {proxy_error(core::ErrorCategory::would_block, "proxy admission"), StatusCode::ServiceUnavailable};
            return;
        }
        admitted_ = true;
        if (context.response_window == 0) {
            rejection_ = {proxy_error(core::ErrorCategory::invalid_argument, "proxy response window"), StatusCode::InternalServerError};
            return;
        }
        capacity_ = std::min(context.response_window, config_.limits.max_read_bytes);
        auto prepared = prepare_outbound(client, config_.forwarding);
        if (!prepared) { rejection_ = {std::move(prepared.error), prepared.status}; return; }
        if (observer_) {
            if (auto hooked = observer_->prepare(*prepared.outbound); !hooked) {
                rejection_ = {hooked.error(), StatusCode::InternalServerError};
                return;
            }
        }
        auto wire = http::serialize_request_head(prepared.outbound->request);
        if (!wire) { rejection_ = {wire.error(), StatusCode::BadRequest}; return; }
        const auto raw = std::as_bytes(std::span(wire.value().data(), wire.value().size()));
        request_ = std::make_shared<const Bytes>(raw.begin(), raw.end());
        resolve_ = std::move(prepared.outbound->resolve);
    }

    ~ForwardTransaction() override {
        if (phase_ != Phase::complete && phase_ != Phase::failed) {
            // Destroyed before completion: the session cancelled us (client
            // loss, stop, or a session-side failure). Late results are ignored
            // by operation ID in the session; the executor retires the socket.
            if (observer_) observer_->on_failure(proxy_error(core::ErrorCategory::connection_aborted, "proxy cancelled"));
            if (stats_ && admitted_) stats_->record(Outcome::cancelled);
        }
        if (stats_ && admitted_) stats_->release();
    }

    http::TransactionStep on_event(const http::TransactionEvent& event) override {
        if (phase_ == Phase::complete || phase_ == Phase::failed) {
            return http::TransactionFailure{proxy_error(core::ErrorCategory::invalid_argument, "proxy finished"), StatusCode::InternalServerError};
        }
        if (std::holds_alternative<http::StartTransaction>(event)) return start();
        if (const auto* drained = std::get_if<http::OutputDrained>(&event)) {
            if (phase_ != Phase::streaming || drained->body_capacity == 0) return fail(StatusCode::InternalServerError, {});
            capacity_ = std::min(drained->body_capacity, config_.limits.max_read_bytes);
            return pump();
        }
        return on_work(std::get<WorkEvent>(event));
    }

private:
    enum class Phase { prepared, resolving, connecting, sending, reading_heads, streaming, validating, complete, failed };
    struct Rejection { core::Error error; StatusCode status{}; };

    http::TransactionStep start() {
        if (phase_ != Phase::prepared) return fail(StatusCode::InternalServerError, {});
        if (rejection_) return fail(rejection_->status, rejection_->error);
        phase_ = Phase::resolving;
        connect_deadline_ = steady_.now() + config_.limits.connect_timeout;
        return work(ResolveWork{resolve_});
    }

    http::TransactionStep on_work(const WorkEvent& result) {
        switch (result.status) {
        case WorkStatus::timed_out: return fail(StatusCode::GatewayTimeout, result.error);
        case WorkStatus::rejected: return fail(StatusCode::ServiceUnavailable, result.error);
        case WorkStatus::failed: case WorkStatus::cancelled: return fail(StatusCode::BadGateway, result.error);
        default: break;
        }
        switch (phase_) {
        case Phase::resolving: {
            if (result.candidates.empty()) return fail(StatusCode::BadGateway, proxy_error(core::ErrorCategory::host_unreachable, "proxy resolve"));
            auto candidates = result.candidates;
            if (candidates.size() > config_.limits.max_candidates) candidates.resize(config_.limits.max_candidates);
            phase_ = Phase::connecting;
            return work(ConnectWork{std::move(candidates)});
        }
        case Phase::connecting:
            stats_->record_origin_connection();
            phase_ = Phase::sending;
            head_deadline_ = steady_.now() + config_.limits.response_timeout;
            return write();
        case Phase::sending:
            if (waiting_) { waiting_ = false; return write(); }
            if (result.status == WorkStatus::would_block) { waiting_ = true; return work(UpstreamWaitWork{net::Interest::write}); }
            if (result.status != WorkStatus::progress || result.transferred == 0 || result.transferred > request_->size() - sent_) {
                return fail(StatusCode::BadGateway, proxy_error(core::ErrorCategory::io_error, "proxy request write"));
            }
            sent_ += result.transferred;
            if (sent_ < request_->size()) return write();
            timing_.request_sent = steady_.now();
            phase_ = Phase::reading_heads;
            return read();
        case Phase::reading_heads: case Phase::streaming:
            if (waiting_) { waiting_ = false; return read(); }
            if (result.status == WorkStatus::would_block) { waiting_ = true; return work(UpstreamWaitWork{net::Interest::read}); }
            if (result.status == WorkStatus::eof) {
                if (!parser_.finish_on_eof()) return fail(StatusCode::BadGateway, parser_.error_detail());
            } else if (result.status == WorkStatus::progress && result.bytes && !result.bytes->empty() &&
                       result.bytes->size() <= config_.limits.max_read_bytes) {
                input_ = result.bytes; consumed_ = 0;
                if (phase_ == Phase::streaming) body_deadline_ = steady_.now() + config_.limits.body_timeout;
            } else return fail(StatusCode::BadGateway, proxy_error(core::ErrorCategory::io_error, "proxy response read"));
            return pump();
        case Phase::validating:
            phase_ = Phase::complete;
            stats_->record(Outcome::completed);
            if (observer_) observer_->on_complete(trailers_);
            return http::ResponseFinished{std::move(trailers_)};
        default:
            return fail(StatusCode::InternalServerError, {});
        }
    }

    // Feeds retained input to the parser. Called only with downstream capacity
    // available, so at most one decoded segment is queued at any time.
    http::TransactionStep pump() {
        for (;;) {
            const auto input = input_ ? std::span<const std::byte>(*input_).subspan(consumed_) : std::span<const std::byte>{};
            const auto event = parser_.consume_stream(input, capacity_);
            consumed_ += event.consumed;
            switch (event.kind) {
            case http::ResponseEventKind::informational_head:
                if (parser_.informational_responses().back().status_code == 101) {
                    return fail(StatusCode::BadGateway, proxy_error(core::ErrorCategory::invalid_argument, "proxy protocol switch"));
                }
                continue; // 100/103 are consumed; the session API carries final heads only.
            case http::ResponseEventKind::trailers: continue;
            case http::ResponseEventKind::error: return fail(StatusCode::BadGateway, parser_.error_detail());
            case http::ResponseEventKind::paused: return fail(StatusCode::InternalServerError, proxy_error(core::ErrorCategory::invalid_argument, "proxy paused"));
            case http::ResponseEventKind::need_more: return read();
            case http::ResponseEventKind::complete:
                // Never reuse the origin, including after close-delimited EOF.
                if (downstream_framing_ == http::BodyFraming::chunked) {
                    trailers_ = forwardable_trailers(parser_.response().trailers, parser_.response().headers);
                }
                input_.reset();
                phase_ = Phase::validating;
                return work(UpstreamCloseWork{});
            case http::ResponseEventKind::body: {
                if (observer_) {
                    if (auto hooked = observer_->on_body(event.body); !hooked) return fail(StatusCode::BadGateway, hooked.error());
                }
                return http::ResponseBody{std::make_shared<const Bytes>(event.body.begin(), event.body.end())};
            }
            case http::ResponseEventKind::final_head: return final_head();
            }
        }
    }

    http::TransactionStep final_head() {
        const auto& origin = parser_.response();
        timing_.final_head_received = steady_.now();
        timing_.final_head_wall = wall_.now();
        auto head = prepare_downstream_head(origin, config_.forwarding);
        if (!head) return fail(StatusCode::BadGateway, head.error());
        if (observer_) {
            if (auto hooked = observer_->on_final_head(origin, head.value(), timing_); !hooked) return fail(StatusCode::BadGateway, hooked.error());
        }
        const auto framing = http::select_response_framing(origin, method_);
        if (framing.error != http::ParseError::none) return fail(StatusCode::BadGateway, parser_.error_detail());
        std::optional<std::uint64_t> length;
        const bool bodyless = method_ == http::Method::HEAD || origin.status_code == 204 || origin.status_code == 304;
        if (bodyless) {
            // Metadata only: keep a valid representation length when the origin gave one.
            const auto declared = http::select_request_framing(origin.headers);
            if (origin.status_code != 204 && declared.error == http::ParseError::none &&
                declared.framing == http::BodyFraming::fixed_length) {
                downstream_framing_ = http::BodyFraming::fixed_length; length = declared.content_length;
            } else downstream_framing_ = http::BodyFraming::none;
        } else if (framing.framing == http::BodyFraming::fixed_length) {
            downstream_framing_ = http::BodyFraming::fixed_length; length = framing.content_length;
        } else {
            // Unknown length: chunked for HTTP/1.1; the session turns it into
            // close-delimited output for HTTP/1.0 clients.
            downstream_framing_ = http::BodyFraming::chunked;
        }
        phase_ = Phase::streaming;
        body_deadline_ = steady_.now() + config_.limits.body_timeout;
        return http::ResponseHead{std::move(head.value()), downstream_framing_, length};
    }

    http::TransactionStep fail(StatusCode status, core::Error error) {
        if (!error) error = proxy_error(core::ErrorCategory::io_error, "proxy transaction");
        if (phase_ != Phase::failed && phase_ != Phase::complete) {
            phase_ = Phase::failed;
            if (stats_ && admitted_) stats_->record(outcome_for(status));
            if (observer_) observer_->on_failure(error);
        }
        return http::TransactionFailure{std::move(error), status};
    }

    core::SteadyTimePoint deadline() const noexcept {
        switch (phase_) {
        case Phase::resolving: case Phase::connecting: return connect_deadline_;
        case Phase::sending: case Phase::reading_heads: return head_deadline_;
        default: return body_deadline_;
        }
    }
    http::TransactionWork work(WorkRequest request) const { return {std::move(request), deadline()}; }
    http::TransactionStep write() {
        return work(UpstreamWriteWork{request_, sent_, std::min(request_->size() - sent_, config_.limits.max_read_bytes)});
    }
    http::TransactionStep read() {
        input_.reset(); consumed_ = 0;
        return work(UpstreamReadWork{std::max<std::size_t>(1, std::min(capacity_, config_.limits.max_read_bytes))});
    }

    ProxyConfig config_;
    std::shared_ptr<ProxyStats> stats_;
    std::unique_ptr<ExchangeObserver> observer_;
    const core::SteadyClock& steady_;
    const core::SystemClock& wall_;
    http::Method method_;
    http::ResponseParser parser_;
    std::optional<Rejection> rejection_;
    net::HostPort resolve_;
    SharedBytes request_, input_;
    std::size_t sent_{}, consumed_{}, capacity_{1};
    core::SteadyTimePoint connect_deadline_{}, head_deadline_{}, body_deadline_{};
    ExchangeTiming timing_;
    http::HeaderMap trailers_;
    http::BodyFraming downstream_framing_{http::BodyFraming::none};
    Phase phase_{Phase::prepared};
    bool admitted_{}, waiting_{};
};

class ForwardFactory final : public http::TransactionFactory {
public:
    ForwardFactory(ProxyConfig config, std::shared_ptr<ProxyStats> stats)
        : config_(std::move(config)), stats_(std::move(stats)) {}
    core::Result<std::unique_ptr<http::Transaction>> create(const http::Request& request, const http::TransactionContext& context) const override {
        return make_forward_transaction(request, context, config_, stats_);
    }
private:
    ProxyConfig config_;
    std::shared_ptr<ProxyStats> stats_;
};
} // namespace

core::Result<void> validate_proxy_config(const ProxyConfig& config) {
    if (auto valid = validate_forwarding_config(config.forwarding); !valid) return valid;
    if (auto valid = http::validate_request_limits(config.limits.response_limits); !valid) return valid;
    const auto& limits = config.limits;
    using std::chrono::milliseconds;
    const auto duration_ok = [](milliseconds value) { return value > milliseconds(0) && value <= milliseconds(10 * 60 * 1000); };
    if (!duration_ok(limits.connect_timeout) || !duration_ok(limits.response_timeout) || !duration_ok(limits.body_timeout) ||
        limits.max_read_bytes == 0 || limits.max_read_bytes > 16 * 1024 * 1024 ||
        limits.max_candidates == 0 || limits.max_candidates > 256 ||
        limits.max_active_transactions == 0 || limits.max_active_transactions > 1'000'000) {
        return proxy_error(core::ErrorCategory::invalid_argument, "proxy config");
    }
    return {};
}

ProxyStats::Snapshot ProxyStats::snapshot() const noexcept {
    const auto get = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    return {get(started_), get(active_), get(peak_active_), get(origin_connections_), get(completed_), get(client_error_),
            get(bad_gateway_), get(gateway_timeout_), get(unavailable_), get(internal_error_), get(cancelled_)};
}

bool ProxyStats::try_admit(std::size_t limit) noexcept {
    auto current = active_.load(std::memory_order_relaxed);
    do {
        if (current >= limit) { unavailable_.fetch_add(1, std::memory_order_relaxed); return false; }
    } while (!active_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel));
    started_.fetch_add(1, std::memory_order_relaxed);
    auto peak = peak_active_.load(std::memory_order_relaxed);
    while (peak < current + 1 && !peak_active_.compare_exchange_weak(peak, current + 1, std::memory_order_relaxed)) {}
    return true;
}

void ProxyStats::release() noexcept { active_.fetch_sub(1, std::memory_order_acq_rel); }

void ProxyStats::record(Outcome outcome) noexcept {
    auto& counter = [&]() -> std::atomic<std::uint64_t>& {
        switch (outcome) {
        case Outcome::completed: return completed_;
        case Outcome::client_error: return client_error_;
        case Outcome::bad_gateway: return bad_gateway_;
        case Outcome::gateway_timeout: return gateway_timeout_;
        case Outcome::unavailable: return unavailable_;
        case Outcome::cancelled: return cancelled_;
        default: return internal_error_;
        }
    }();
    counter.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<http::Transaction> make_forward_transaction(
    const http::Request& client, const http::TransactionContext& context, const ProxyConfig& config,
    std::shared_ptr<ProxyStats> stats, std::unique_ptr<ExchangeObserver> observer) {
    return std::make_unique<ForwardTransaction>(client, context, config, std::move(stats), std::move(observer));
}

core::Result<std::shared_ptr<const http::TransactionFactory>> make_forward_factory(ProxyConfig config, std::shared_ptr<ProxyStats> stats) {
    if (auto valid = validate_proxy_config(config); !valid) return valid.error();
    if (!stats) stats = std::make_shared<ProxyStats>();
    return std::shared_ptr<const http::TransactionFactory>(std::make_shared<ForwardFactory>(std::move(config), std::move(stats)));
}

} // namespace proxy
