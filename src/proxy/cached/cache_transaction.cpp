#include "proxy/cache_transaction.hpp"

#include <algorithm>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

namespace proxy {
namespace {
using cache::StorageOutcome;
using http::StatusCode;

struct Services {
    std::shared_ptr<cache::storage::LruStorage> storage;
    CacheProxyConfig config;
    cache::policy::CachePolicy policy;
    std::shared_ptr<CacheStats> stats;
    std::shared_ptr<ProxyStats> forwarding_stats;
    std::mutex mutex;
    std::size_t active{};
    // Pointers borrow transaction-owned keys. Admission bounds this preallocated
    // registry; unregister before the owning key dies. No bodies are held here.
    std::vector<const cache::Key*> misses;

    bool admit() {
        std::lock_guard lock(mutex);
        if (active == config.forwarding.limits.max_active_transactions) return false;
        ++active;
        return true;
    }
    void track(const cache::Key& key) {
        std::lock_guard lock(mutex);
        if (std::any_of(misses.begin(), misses.end(), [&](const auto* other) { return *other == key; }))
            stats->record(CacheEvent::duplicate_miss);
        misses.push_back(&key);
    }
    void release(const cache::Key& key) {
        std::lock_guard lock(mutex);
        misses.erase(std::remove(misses.begin(), misses.end(), &key), misses.end());
        --active;
    }
};

class CacheTransaction final : public http::Transaction {
public:
    CacheTransaction(const http::Request& request, const http::TransactionContext& context,
                     std::shared_ptr<Services> services)
        : request_(request), context_(context), services_(std::move(services)), admitted_(services_->admit()) {}

    ~CacheTransaction() override {
        // The observer borrows this object. Destroy forwarding first so its
        // cancellation callback still has live state, then release pins/builders.
        forward_.reset();
        if (admitted_) {
            if (!finished_) record(CacheEvent::cancelled);
            services_->release(key_);
        }
    }

    http::TransactionStep on_event(const http::TransactionEvent& event) override {
        if (finished_) return failure(StatusCode::InternalServerError, "cache transaction finished");
        if (!started_) {
            started_ = true;
            if (!admitted_) { record(CacheEvent::rejected); return failure(StatusCode::ServiceUnavailable, "cache admission"); }
            if (!std::holds_alternative<http::StartTransaction>(event) || context_.response_window == 0)
                return failure(StatusCode::InternalServerError, "cache transaction start");
            return start();
        }
        if (serving_) {
            const auto* drained = std::get_if<http::OutputDrained>(&event);
            if (!drained || !drained->body_capacity) return failure(StatusCode::InternalServerError, "cache output capacity");
            const auto bytes = selected_->body->bytes();
            if (offset_ == bytes.size()) {
                selected_.reset(); finished_ = true;
                return http::ResponseFinished{};
            }
            const auto count = std::min({drained->body_capacity, services_->config.forwarding.limits.max_read_bytes, bytes.size() - offset_});
            const auto segment = bytes.subspan(offset_, count);
            offset_ += count;
            return http::ResponseBody{std::make_shared<const concurrency::Bytes>(segment.begin(), segment.end())};
        }
        return inspect(forward_->on_event(event));
    }

private:
    class Observer final : public ExchangeObserver {
    public:
        explicit Observer(CacheTransaction& owner) : owner_(owner) {}
        core::Result<void> prepare(OutboundRequest& outbound) override {
            for (const auto& field : owner_.conditions_) outbound.request.headers.set(field.name, field.value);
            return {};
        }
        core::Result<void> on_final_head(const http::Response& origin, http::Response&, const ExchangeTiming& timing) override {
            owner_.head(origin, timing); return {};
        }
        core::Result<void> on_body(std::span<const std::byte> bytes) override {
            owner_.body_bytes_ += bytes.size();
            if (owner_.candidate_ && !owner_.candidate_->append(bytes)) owner_.discard();
            return {};
        }
        void on_complete(const http::HeaderMap& trailers) override { owner_.complete(trailers); }
        void on_failure(const core::Error&) override {
            owner_.discard(); owner_.invalidate_changed();
            owner_.validation_failed();
        }
    private:
        CacheTransaction& owner_;
    };

    void record(CacheEvent event) { services_->stats->record(event); }
    void validation_failed() {
        if (validating_ && !validation_failure_recorded_) {
            record(CacheEvent::validation_failed); validation_failure_recorded_ = true;
        }
    }
    cache::PolicyDecision decision(const cache::Snapshot& snapshot) const {
        return services_->policy.evaluate(request_, snapshot, context_.steady_clock.now(), context_.wall_clock.now());
    }
    http::TransactionStep failure(StatusCode status, std::string operation) {
        finished_ = true;
        candidate_.reset(); selected_.reset();
        return http::TransactionFailure{core::make_error(core::ErrorCategory::io_error, std::move(operation)), status};
    }
    http::TransactionStep start() {
        auto prepared = prepare_outbound(request_, services_->config.forwarding.forwarding);
        if (!prepared) { finished_ = true; return http::TransactionFailure{prepared.error, prepared.status}; }
        key_ = std::move(prepared.outbound->target);
        auto policy = decision({});
        cache_request_ = policy.lookup_allowed;
        if (policy.lookup_allowed) {
            const auto found = services_->storage->lookup(key_);
            selected_ = found.entry; observed_ = found.observed;
            // Reader exhaustion/errors disable insertion as well as reuse. An
            // error is not a trustworthy observation of absence.
            lookup_valid_ = found.outcome == StorageOutcome::hit || found.outcome == StorageOutcome::miss;
            policy = decision(selected_);
        }
        if (policy.kind == cache::DecisionKind::hit) { record(CacheEvent::hit); return cached_head(); }
        if (!policy.network_allowed) {
            record(CacheEvent::cache_only_failure);
            return failure(StatusCode::GatewayTimeout, "cache-only unavailable");
        }
        allow_store_ = policy.store_allowed && lookup_valid_;
        if (policy.kind == cache::DecisionKind::bypass) record(CacheEvent::bypass);
        else if (policy.kind == cache::DecisionKind::revalidate) {
            validating_ = true; conditions_ = std::move(policy.conditional_headers);
            record(CacheEvent::validation_request);
        } else {
            record(CacheEvent::miss);
            if (!selected_) services_->track(key_);
        }
        return begin_forward();
    }
    http::TransactionStep begin_forward() {
        origin_ = {}; body_bytes_ = 0; changed_ = false;
        validation_failure_recorded_ = false;
        forward_ = make_forward_transaction(request_, context_, services_->config.forwarding,
            services_->forwarding_stats, allow_store_ ? std::make_unique<Observer>(*this) : nullptr);
        return inspect(forward_->on_event(http::StartTransaction{}));
    }
    http::TransactionStep cached_head() {
        http::Response response;
        response.headers = selected_->headers;
        auto head = prepare_downstream_head(response, services_->config.forwarding.forwarding);
        if (!head) return failure(StatusCode::BadGateway, "cached response headers");
        head.value().headers.set("Age", cache::policy::age_header(decision(selected_).current_age));
        serving_ = true; offset_ = 0;
        return http::ResponseHead{std::move(head.value()), http::BodyFraming::fixed_length, selected_->body->bytes().size()};
    }
    void discard() {
        if (candidate_) { candidate_.reset(); record(CacheEvent::discarded); }
    }
    void conflict(const cache::StoreResult& result) {
        if (result.outcome == StorageOutcome::version_conflict) record(CacheEvent::version_conflict);
    }
    void invalidate_changed() {
        if (!changed_ || !selected_) return;
        conflict(services_->storage->erase(key_, observed_));
        changed_ = false;
    }
    void head(const http::Response& origin, const ExchangeTiming& timing) {
        origin_ = origin; timing_ = timing;
        changed_ = allow_store_ && selected_ && origin.status_code >= 200 && origin.status_code < 500 && origin.status_code != 304;
        if (validating_ && origin.status_code == 200) record(CacheEvent::validation_200);
        else if (origin.status_code != 304) validation_failed();
        const auto framing = http::select_response_framing(origin, request_.method);
        framing_ = framing.framing;
        if (!allow_store_ || framing.error != http::ParseError::none ||
            !services_->policy.can_store_head(request_, origin, framing_)) return;
        auto headers = end_to_end_fields(origin.headers);
        if (!headers) return;
        auto candidate = services_->storage->begin(key_, headers.value(), timing);
        if (candidate) candidate_.emplace(std::move(candidate.value()));
        else record(CacheEvent::discarded);
    }
    void complete(const http::HeaderMap& trailers) {
        origin_.trailers = trailers;
        if (candidate_ && services_->policy.can_store_stream(request_, origin_, framing_, true, body_bytes_)) {
            auto finished = candidate_->finish();
            candidate_.reset();
            if (finished) {
                const auto result = services_->storage->replace(key_, observed_, std::move(finished.value()));
                if (result.outcome == StorageOutcome::stored) {
                    record(CacheEvent::stored); changed_ = false;
                } else { conflict(result); record(CacheEvent::discarded); }
            } else record(CacheEvent::discarded);
        } else discard();
        invalidate_changed();
        if (origin_.status_code != 304) selected_.reset();
    }
    http::TransactionStep inspect(http::TransactionStep step) {
        if (const auto* head = std::get_if<http::ResponseHead>(&step)) {
            if (head->response.status_code == 304 && cache_request_) return validation();
            if (request_.method == http::Method::HEAD || head->response.status_code == 204 || head->response.status_code == 304)
                finished_ = true;
        }
        if (std::holds_alternative<http::ResponseFinished>(step) || std::holds_alternative<http::TransactionFailure>(step)) {
            finished_ = true; selected_.reset(); candidate_.reset();
        }
        return step;
    }
    http::TransactionStep validation() {
        // B has completed and closed its origin before returning a bodyless head.
        forward_.reset();
        if (validating_ && selected_) {
            auto merged = services_->policy.revalidate(selected_, origin_, timing_);
            if (merged && merged.value().outcome == cache::ValidationOutcome::refresh_metadata) {
                auto refreshed = services_->storage->refresh(selected_, merged.value().merged_headers, timing_);
                if (refreshed) {
                    const auto replaced = services_->storage->replace(key_, observed_, refreshed.value());
                    if (replaced.outcome == StorageOutcome::stored) {
                        selected_ = std::move(refreshed.value());
                        record(CacheEvent::validation_304);
                        return cached_head();
                    }
                    conflict(replaced);
                }
            } else if (merged && merged.value().outcome == cache::ValidationOutcome::reject) {
                conflict(services_->storage->erase(key_, observed_));
            }
            validation_failed();
        }
        if (!validating_ || retried_) return failure(StatusCode::BadGateway, "unusable origin 304");
        retried_ = true; validating_ = false; conditions_.clear(); selected_.reset();
        const auto current = services_->storage->lookup(key_);
        selected_ = current.entry; observed_ = current.observed;
        allow_store_ = current.outcome == StorageOutcome::hit || current.outcome == StorageOutcome::miss;
        return begin_forward();
    }

    const http::Request& request_;
    http::TransactionContext context_;
    std::shared_ptr<Services> services_;
    cache::Key key_;
    cache::EntryVersion observed_;
    cache::Snapshot selected_;
    std::optional<cache::storage::Candidate> candidate_;
    http::HeaderMap conditions_;
    http::Response origin_;
    ExchangeTiming timing_;
    http::BodyFraming framing_{http::BodyFraming::none};
    std::uint64_t body_bytes_{};
    std::size_t offset_{};
    bool admitted_{}, started_{}, finished_{}, serving_{}, lookup_valid_{}, allow_store_{};
    bool validating_{}, retried_{}, changed_{}, cache_request_{}, validation_failure_recorded_{};
    std::unique_ptr<http::Transaction> forward_;
};

class CacheFactory final : public http::TransactionFactory {
public:
    explicit CacheFactory(std::shared_ptr<Services> services) : services_(std::move(services)) {}
    core::Result<std::unique_ptr<http::Transaction>> create(const http::Request& request, const http::TransactionContext& context) const override {
        return std::unique_ptr<http::Transaction>(std::make_unique<CacheTransaction>(request, context, services_));
    }
private:
    std::shared_ptr<Services> services_;
};
} // namespace

core::Result<std::shared_ptr<const http::TransactionFactory>> make_cache_factory(
    std::shared_ptr<cache::storage::LruStorage> storage, CacheProxyConfig config,
    std::shared_ptr<CacheStats> stats, std::shared_ptr<ProxyStats> forwarding_stats) try {
    if (!storage || config.policy.object_bytes == 0)
        return core::make_error(core::ErrorCategory::invalid_argument, "cache proxy config");
    if (auto valid = validate_proxy_config(config.forwarding); !valid) return valid.error();
    auto services = std::make_shared<Services>();
    services->storage = std::move(storage);
    services->config = std::move(config);
    services->policy = cache::policy::CachePolicy(services->config.policy);
    services->stats = stats ? std::move(stats) : std::make_shared<CacheStats>();
    services->forwarding_stats = forwarding_stats ? std::move(forwarding_stats) : std::make_shared<ProxyStats>();
    services->misses.reserve(services->config.forwarding.limits.max_active_transactions);
    return std::shared_ptr<const http::TransactionFactory>(std::make_shared<CacheFactory>(std::move(services)));
} catch (const std::bad_alloc&) {
    return core::make_error(core::ErrorCategory::system_error, "cache proxy allocation");
}
} // namespace proxy
