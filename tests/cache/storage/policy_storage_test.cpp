#include "cache/storage/lru_storage.hpp"
#include "cache/policy/cache_policy.hpp"
#include "http/date.hpp"
#include "test_support.hpp"

int main() {
    using namespace cache;
    using namespace std::chrono_literals;
    auto store = test::take(storage::LruStorage::create());
    policy::CachePolicy policy;
    const auto key = test::take(http::parse_target("http://example.com/object"));
    const auto wall = test::take(http::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"));
    http::Request request; request.target = key.key();
    http::HeaderMap headers;
    headers.add("Date", http::format_http_date(wall));
    headers.add("Cache-Control", "max-age=1");
    headers.add("ETag", "\"old\"");
    auto make = [&](std::string_view payload) {
        auto builder = test::take(store->begin(key, headers, {{}, {}, wall}));
        CHECK(builder.append(std::as_bytes(std::span(payload.data(), payload.size()))));
        return test::take(builder.finish());
    };
    CHECK(store->replace(key, store->lookup(key).observed, make("old body")).outcome == StorageOutcome::stored);
    auto validation = store->lookup(key);
    auto decision = policy.evaluate(request, validation.entry, core::SteadyTimePoint{} + 2s, wall);
    CHECK(decision.kind == DecisionKind::revalidate);
    CHECK(decision.conditional_headers.get("If-None-Match") == "\"old\"");

    // A competing successful fetch replaces the representation before the 304.
    headers.set("ETag", "\"new\"");
    CHECK(store->replace(key, validation.observed, make("new body")).outcome == StorageOutcome::stored);
    http::Response not_modified(http::StatusCode::NotModified);
    not_modified.headers.add("ETag", "\"old\"");
    not_modified.headers.add("Date", http::format_http_date(wall + 2s));
    not_modified.headers.add("Cache-Control", "max-age=100");
    auto update = test::take(policy.revalidate(validation.entry, not_modified,
        {core::SteadyTimePoint{} + 1s, core::SteadyTimePoint{} + 2s, wall + 2s}));
    CHECK(update.outcome == ValidationOutcome::refresh_metadata);
    auto candidate = test::take(store->refresh(validation.entry, update.merged_headers, update.timing));
    CHECK(candidate->body == validation.entry->body);
    CHECK(store->replace(key, update.expected, std::move(candidate)).outcome == StorageOutcome::version_conflict);
    auto current = store->lookup(key);
    CHECK(current.entry->headers.get("ETag") == "\"new\"");
    CHECK(current.entry->body->bytes()[0] == std::byte{'n'});
    CHECK(validation.entry->body->bytes()[0] == std::byte{'o'});

    // The same version guard protects invalidation after changed eligibility.
    not_modified.headers.add("Vary", "Accept-Encoding");
    auto invalidated = test::take(policy.revalidate(validation.entry, not_modified, update.timing));
    CHECK(invalidated.outcome == ValidationOutcome::reject);
    CHECK(store->erase(key, invalidated.expected).outcome == StorageOutcome::version_conflict);
    CHECK(store->lookup(key).entry->version == current.observed);

    // Normal validation refreshes metadata while retaining the exact body.
    not_modified.headers.remove("Vary"); not_modified.headers.set("ETag", "\"new\"");
    auto fresh = test::take(policy.revalidate(current.entry, not_modified, update.timing));
    auto refreshed = test::take(store->refresh(current.entry, fresh.merged_headers, fresh.timing));
    CHECK(store->replace(key, fresh.expected, std::move(refreshed)).outcome == StorageOutcome::stored);
    auto hit = store->lookup(key);
    CHECK(hit.entry->body == current.entry->body);
    auto usable = policy.evaluate(request, hit.entry, update.timing.final_head_received + 1s, wall);
    CHECK(usable.kind == DecisionKind::hit && !usable.network_allowed && usable.current_age == 2s);
    CHECK(store->clear());
    CHECK(hit.entry->body->bytes()[0] == std::byte{'n'});
}
