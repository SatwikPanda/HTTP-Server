#include "fixtures.hpp"

#include <barrier>

namespace {
using namespace cached_test;

void duplicate_misses_and_validation_conflicts() {
    Environment env;
    Exchange first(env, {response("first")}), second(env, {response("second")});
    first.until_head(); second.until_head(); second.finish(); first.finish();
    CHECK(first.output == "first" && second.output == "second" && text(env.entry()->body->bytes()) == "second");
    CHECK(env.stats->count(CacheEvent::duplicate_miss) == 1 && env.stats->count(CacheEvent::version_conflict) == 1);
    env.steady.advance(61s);
    Exchange a(env, {validation("ETag: \"v1\"\r\nCache-Control: max-age=120\r\nX-Meta: a\r\n")});
    Exchange b(env, {validation("ETag: \"v1\"\r\nX-Meta: b\r\n"), response("third", "Cache-Control: max-age=120\r\nX-Meta: third\r\n")});
    a.finish(); b.finish();
    CHECK(a.output == "second" && a.heads[0].response.headers.get("X-Meta") == "a");
    CHECK(b.output == "third" && b.heads[0].response.headers.get("X-Meta") == "third" && b.resolves == 2);
    CHECK(text(env.entry()->body->bytes()) == "third" && env.entry()->headers.get("X-Meta") == "third");
    CHECK(env.stats->count(CacheEvent::version_conflict) == 2);
}

void changed_response_cannot_erase_new_version() {
    Environment env; fill(env); env.steady.advance(61s);
    Exchange changed(env, {response("private", "Cache-Control: private\r\n")}); changed.until_head();
    Exchange validate(env, {validation()}); validate.finish();
    const auto newer = env.entry();
    changed.finish();
    CHECK(changed.output == "private" && env.entry()->version == newer->version);
    CHECK(text(env.entry()->body->bytes()) == "old" && env.stats->count(CacheEvent::version_conflict) == 1);
    // Also exercise a complete eligible 200 losing CAS, and its invalidation
    // fallback losing CAS; neither may overwrite/delete a newer representation.
    env.steady.advance(121s);
    Exchange late(env, {response("late")}); late.until_head();
    Exchange winner(env, {response("winner")}); winner.finish();
    late.finish(); CHECK(text(env.entry()->body->bytes()) == "winner");
}

void pinned_eviction_admission_and_release() {
    auto limits = cache::storage::Limits{}; limits.entries = 1;
    Environment env(limits); fill(env, response("pinned"));
    Exchange slow(env); slow.until_head();
    fill(env, response("other"), "/other");
    CHECK(!env.entry() && env.storage->statistics().retained_bytes > 0);
    CHECK(env.storage->statistics().evictions == 1);
    slow.body_window = 1; slow.finish();
    CHECK(slow.output == "pinned" && env.storage->statistics().retained_bytes == 0);
    CHECK(env.storage->clear());
    CHECK(env.storage->statistics().evictions == 1);
    CHECK(env.storage->statistics().readers == 0 && env.storage->statistics().candidate_bytes == 0);

    proxy::CacheProxyConfig config; config.forwarding.limits.max_active_transactions = 1;
    Environment bounded({}, config); fill(bounded);
    auto held = std::make_unique<Exchange>(bounded); held->until_head();
    { Exchange rejected(bounded); rejected.finish(); CHECK(rejected.failure == 503 && rejected.resolves == 0); }
    held.reset();
    Exchange accepted(bounded); accepted.finish(); CHECK(accepted.output == "old");
    CHECK(bounded.stats->count(CacheEvent::cancelled) == 1 && bounded.stats->count(CacheEvent::rejected) == 1);
}

void concurrent_candidates_stay_bounded() {
    auto limits = cache::storage::Limits{};
    limits.entries = 4; limits.resident_bytes = 12000; limits.total_bytes = 18000;
    limits.object_bytes = 7000; limits.metadata_bytes = 5000;
    Environment env(limits);
    const auto baseline = env.storage->statistics().total_bytes;
    constexpr unsigned count = 6;
    std::barrier gate(count + 1);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < count; ++i) workers.emplace_back([&, i] {
        const std::string body(6000, static_cast<char>('a' + i));
        Exchange exchange(env, {response(body)}, {}, "/concurrent" + std::to_string(i));
        exchange.until_head(); exchange.advance();
        gate.arrive_and_wait(); gate.arrive_and_wait();
        while (!exchange.done) {
            exchange.advance();
            const auto current = env.storage->statistics();
            CHECK(current.total_bytes <= limits.total_bytes && current.resident_bytes <= limits.resident_bytes);
        }
        CHECK(!exchange.failure && exchange.output == body);
    });
    gate.arrive_and_wait();
    CHECK(env.storage->statistics().candidate_bytes > 0);
    gate.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    CHECK(env.storage->clear());
    const auto after = env.storage->statistics();
    CHECK(after.total_bytes == baseline && after.readers == 0 && after.retained_bytes == 0 && after.candidate_bytes == 0);
}

void parallel_hits_survive_eviction() {
    auto limits = cache::storage::Limits{}; limits.entries = 1;
    Environment env(limits); const std::string body(900, 'p'); fill(env, response(body));
    std::barrier gate(5);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&] {
        Exchange hit(env); hit.until_head();
        gate.arrive_and_wait(); gate.arrive_and_wait();
        hit.body_window = 1; hit.finish();
        CHECK(hit.resolves == 0 && hit.output == body);
    });
    gate.arrive_and_wait();
    CHECK(env.storage->statistics().readers == 4);
    fill(env, response("replacement"), "/other");
    CHECK(env.storage->statistics().retained_bytes > 0 && env.storage->statistics().evictions == 1);
    gate.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    CHECK(env.storage->statistics().readers == 0 && env.storage->statistics().retained_bytes == 0);
    CHECK(env.stats->count(CacheEvent::hit) == 4);
}

void gated_real_origin_loses_to_competing_completion() {
    auto runtime = test::take(net::initialize_network());
    test::ControlledOrigin origin({response("late network")}, 3, true);
    const auto url = "http://" + origin.target().host + ":" + std::to_string(origin.target().port) + "/race";
    Environment env;
    proxy_test::Channel downstream(proxy_test::get(url), 3);
    http::HttpSessionStats result;
    std::thread server([&] { result = http::run_http_session(downstream, session(env)); });
    origin.wait_for_request();
    // Response release is the barrier. The competing transaction uses scripted
    // I/O but the identical production factory, key, parser, policy and storage.
    fill(env, response("winner"), url);
    origin.release(); server.join(); origin.join();
    CHECK(result.completed_cleanly);
    const auto parsed = proxy_test::parse_all(downstream.output, {http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].response.body == "late network");
    const auto saved = env.storage->lookup(test::take(http::parse_target(url))).entry;
    CHECK(saved && text(saved->body->bytes()) == "winner");
    CHECK(env.stats->count(CacheEvent::duplicate_miss) == 1 && env.stats->count(CacheEvent::version_conflict) == 1);
}
} // namespace

int main() {
    duplicate_misses_and_validation_conflicts(); changed_response_cannot_erase_new_version();
    pinned_eviction_admission_and_release(); concurrent_candidates_stay_bounded(); parallel_hits_survive_eviction();
    gated_real_origin_loses_to_competing_completion();
    std::cout << "cache proxy concurrency acceptance passed\n";
}
