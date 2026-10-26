#include "cache/storage/lru_storage.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <limits>
#include <new>

// One-shot allocation failure exercises the real containers and rollback paths.
namespace { thread_local int fail_after = -1; }
void* operator new(std::size_t size) {
    if (fail_after >= 0 && fail_after-- == 0) throw std::bad_alloc();
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace cache;
using namespace cache::storage;
Key key(std::string_view path) { return test::take(http::parse_target(path, "example.com")); }
std::span<const std::byte> bytes(std::string_view value) { return std::as_bytes(std::span(value.data(), value.size())); }
Snapshot candidate(LruStorage& store, const Key& target, std::string_view value = "abc") {
    auto builder = test::take(store.begin(target, {}, {}));
    CHECK(builder.append(bytes(value)));
    return test::take(builder.finish());
}
void insert(LruStorage& store, const Key& target, std::string_view value = "abc") {
    auto entry = candidate(store, target, value);
    CHECK(store.replace(target, store.lookup(target).observed, std::move(entry)).outcome == StorageOutcome::stored);
}
void identity_lru_and_versions() {
    auto store = test::take(LruStorage::create({2, 20000, 100000, 1000, 10, 10000}));
    auto a = key("/a"), b = key("/b"), c = key("/c");
    const auto absent = store->lookup(a).observed;
    insert(*store, a); insert(*store, b);
    auto held = store->lookup(a);
    CHECK(held.outcome == StorageOutcome::hit);
    insert(*store, c);
    CHECK(store->lookup(b).outcome == StorageOutcome::miss);
    CHECK(store->lookup(a).outcome == StorageOutcome::hit);
    CHECK(store->replace(a, absent, candidate(*store, a)).outcome == StorageOutcome::version_conflict);
    CHECK(store->erase(a, held.observed).outcome == StorageOutcome::stored);
    insert(*store, a, "new");
    CHECK(store->erase(a, held.observed).outcome == StorageOutcome::version_conflict);
    CHECK(store->replace(a, held.observed, candidate(*store, a)).outcome == StorageOutcome::version_conflict);
    CHECK(held.entry->body->bytes()[0] == std::byte{'a'});
    CHECK(store->statistics().retained_bytes > 0);
    held.entry.reset();
    CHECK(store->statistics().retained_bytes == 0);

    auto normalized = test::take(http::parse_target("HTTP://EXAMPLE.COM:80/a"));
    CHECK(normalized == a && KeyHash{}(normalized) == KeyHash{}(a));
    for (const auto target : {"/a?x=1&y=2", "/a?y=2&x=1", "/%61", "/%6A", "/%6a", "/a?", "/a//"}) {
        CHECK(key(target) != a);
    }
    CHECK(key("/a?x=1&y=2") != key("/a?y=2&x=1"));
    CHECK(key("/%6A") != key("/%6a"));
    auto old_miss = store->lookup(b).observed;
    CHECK(store->clear());
    CHECK(store->replace(b, old_miss, candidate(*store, b)).outcome == StorageOutcome::version_conflict);
    CHECK(store->statistics().entries == 0 && store->statistics().resident_bytes == 0);
}
void limits_and_lifetime() {
    auto store = test::take(LruStorage::create({3, 20000, 100000, 5, 1, 10000}));
    const auto baseline = store->statistics().total_bytes;
    auto target = key("/a");
    {
        auto builder = test::take(store->begin(target, {}, {}));
        CHECK(builder.append(bytes("123")));
        CHECK(builder.append(bytes("45")));
        CHECK(!builder.append(bytes("6")));
        auto entry = test::take(builder.finish());
        CHECK(!builder.append(bytes("x")) && !builder.finish());
        CHECK(entry->body->bytes().size() == 5);
        CHECK(store->replace(target, store->lookup(target).observed, entry).outcome == StorageOutcome::stored);
    }
    auto first = store->lookup(target);
    CHECK(first.outcome == StorageOutcome::hit);
    CHECK(store->lookup(target).outcome == StorageOutcome::exhausted);
    auto body = first.entry->body;
    auto copy = first.entry;
    CHECK(store->clear());
    first.entry.reset(); copy.reset();
    CHECK(store->statistics().readers == 0);
    CHECK(store->statistics().retained_bytes == body->accounted_bytes());
    body.reset();
    CHECK(store->statistics().total_bytes == baseline);
    { auto cancelled = test::take(store->begin(target, {}, {})); CHECK(cancelled.append(bytes("123"))); }
    CHECK(store->statistics().total_bytes == baseline);
    auto forged = std::make_shared<Entry>();
    CHECK(store->replace(target, store->lookup(target).observed, forged).outcome == StorageOutcome::failed);
    CHECK(!store->refresh(forged, {}, {}));
    http::HeaderMap bad; bad.add("Connection", "close");
    CHECK(!store->begin(target, bad, {}));
    bad.clear(); bad.add("Large", std::string(10000, 'x'));
    CHECK(!store->begin(target, bad, {}));

    insert(*store, target, "");
    auto empty = store->lookup(target);
    CHECK(empty.entry->body->bytes().empty());
    store.reset();
    CHECK(empty.entry->body->bytes().empty());
}
void exact_resident_and_total() {
    auto probe = test::take(LruStorage::create({3, 10000, 30000, 100, 10, 10000}));
    auto a = key("/a"), b = key("/b");
    insert(*probe, a);
    const auto cost = probe->statistics().resident_bytes;
    auto store = test::take(LruStorage::create({3, cost, 30000, 100, 10, 10000}));
    insert(*store, a); insert(*store, b);
    CHECK(store->statistics().resident_bytes == cost);
    CHECK(store->lookup(a).outcome == StorageOutcome::miss);
    CHECK(store->replace(b, store->lookup(b).observed, candidate(*store, b, "abcd")).outcome == StorageOutcome::oversized);
    CHECK(store->lookup(b).outcome == StorageOutcome::hit);

    const auto fixed = store->statistics().total_bytes - cost;
    const auto empty_cost = cost - 3;
    auto tight = test::take(LruStorage::create({3, empty_cost, fixed + empty_cost, 100, 10, 10000}));
    auto entry = candidate(*tight, a, "");
    CHECK(tight->statistics().total_bytes == fixed + empty_cost);
    CHECK(!tight->begin(b, {}, {}));
    CHECK(tight->replace(a, tight->lookup(a).observed, entry).outcome == StorageOutcome::exhausted);
    entry.reset();
    CHECK(tight->statistics().total_bytes == fixed);
}
void refresh_body_and_failures() {
    auto store = test::take(LruStorage::create({3, 20000, 100000, 1000, 10, 10000}));
    auto target = key("/large-name-for-map-allocation");
    insert(*store, target);
    fail_after = 0;
    auto failed_pin = store->lookup(target);
    fail_after = -1;
    CHECK(failed_pin.outcome == StorageOutcome::failed && store->statistics().readers == 0);
    auto selected = store->lookup(target);
    http::HeaderMap headers; headers.add("ETag", "W/\"v1\"");
    auto refreshed = test::take(store->refresh(selected.entry, headers, {}));
    CHECK(refreshed->body == selected.entry->body);
    auto result = store->replace(target, selected.observed, refreshed);
    CHECK(result.outcome == StorageOutcome::stored && result.version != selected.observed);
    CHECK(selected.entry->headers.empty());
    refreshed.reset(); selected.entry.reset();
    CHECK(store->statistics().retained_bytes == 0);
    CHECK(store->clear());
    const auto baseline = store->statistics().total_bytes;
    {
        auto growth = test::take(store->begin(target, {}, {}));
        CHECK(growth.append(bytes("first")));
        const auto before = store->statistics().total_bytes;
        fail_after = 0;
        auto failed = growth.append(bytes("second"));
        fail_after = -1;
        CHECK(!failed && store->statistics().total_bytes == before);
        CHECK(test::take(growth.finish())->body->bytes().size() == 5);
    }
    // All actual allocation points in begin, append, finish and insert are failed
    // in turn. A one-shot hook lets Result's diagnostic allocation still succeed.
    for (int point = 0; point < 45; ++point) {
        fail_after = point;
        auto built = store->begin(target, headers, {});
        if (built) {
            auto grown = built->append(bytes("payload"));
            if (grown) {
                auto finished = built->finish();
                if (finished) {
                    auto stored = store->replace(target, store->lookup(target).observed, std::move(finished.value()));
                    CHECK(stored.outcome == StorageOutcome::stored || stored.outcome == StorageOutcome::failed);
                }
            }
        }
        fail_after = -1;
        built = core::make_error(core::ErrorCategory::invalid_argument, "test");
        CHECK(store->clear());
        CHECK(store->statistics().total_bytes == baseline);
    }
    for (int point = 0; point < 30; ++point) {
        insert(*store, target, "old");
        const auto version = store->lookup(target).observed;
        auto next = candidate(*store, target, "new");
        fail_after = point;
        auto replaced = store->replace(target, version, std::move(next));
        fail_after = -1;
        auto current = store->lookup(target);
        CHECK(current.outcome == StorageOutcome::hit);
        if (replaced.outcome == StorageOutcome::failed) {
            CHECK(current.observed == version && current.entry->body->bytes()[0] == std::byte{'o'});
        } else {
            CHECK(replaced.outcome == StorageOutcome::stored && current.entry->body->bytes()[0] == std::byte{'n'});
        }
        current.entry.reset();
        fail_after = point;
        auto erased = store->erase(target, current.observed);
        fail_after = -1;
        CHECK(erased.outcome == StorageOutcome::stored || erased.outcome == StorageOutcome::failed);
        if (erased.outcome == StorageOutcome::failed) CHECK(store->lookup(target).observed == current.observed);
        fail_after = point;
        auto cleared = store->clear();
        fail_after = -1;
        if (!cleared) CHECK(cleared.error().category == core::ErrorCategory::system_error);
        CHECK(store->clear());
        CHECK(store->statistics().total_bytes == baseline);
    }
}
} // namespace

int main() { identity_lru_and_versions(); limits_and_lifetime(); exact_resident_and_total(); refresh_body_and_failures(); }
