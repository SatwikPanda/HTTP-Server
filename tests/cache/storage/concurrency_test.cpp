#include "cache/storage/lru_storage.hpp"
#include "test_support.hpp"

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

int main() {
    using namespace cache;
    auto store = test::take(storage::LruStorage::create({8, 100000, 1000000, 1000, 64, 10000}));
    const auto baseline = store->statistics().total_bytes;
    const auto key = test::take(http::parse_target("http://example.com/data"));
    std::barrier start(8);
    std::atomic<int> wins{};
    std::vector<std::thread> threads;
    const auto observed = store->lookup(key).observed;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&, i] {
        auto builder = test::take(store->begin(key, {}, {}));
        const std::byte value{static_cast<unsigned char>(i)};
        CHECK(builder.append(std::span(&value, 1)));
        auto candidate = test::take(builder.finish());
        start.arrive_and_wait();
        auto result = store->replace(key, observed, std::move(candidate));
        if (result.outcome == StorageOutcome::stored) ++wins;
        else CHECK(result.outcome == StorageOutcome::version_conflict);
        start.arrive_and_wait();
        for (int iteration = 0; iteration < 200; ++iteration) {
            auto snapshot = store->lookup(key);
            if (snapshot.outcome == StorageOutcome::hit) {
                const auto original = snapshot.entry->body->bytes()[0];
                auto replacement = test::take(store->refresh(snapshot.entry, {}, {}));
                auto updated = store->replace(key, snapshot.observed, std::move(replacement));
                CHECK(updated.outcome == StorageOutcome::stored || updated.outcome == StorageOutcome::version_conflict);
                CHECK(snapshot.entry->body->bytes()[0] == original);
            } else {
                CHECK(snapshot.outcome == StorageOutcome::miss);
                auto next = test::take(store->begin(key, {}, {}));
                CHECK(next.append(std::span(&value, 1)));
                auto inserted = store->replace(key, snapshot.observed, test::take(next.finish()));
                CHECK(inserted.outcome == StorageOutcome::stored || inserted.outcome == StorageOutcome::version_conflict);
            }
            if (i == 0 && iteration % 5 == 0) CHECK(store->clear());
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(wins == 1);
    CHECK(store->clear());
    CHECK(store->statistics().readers == 0);
    CHECK(store->statistics().total_bytes == baseline);
}
