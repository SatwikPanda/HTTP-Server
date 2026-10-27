#include "fixtures.hpp"

#include "concurrency/single_session_driver.hpp"

namespace {
using namespace cached_test;

void miss_hit_age_and_cache_only() {
    Environment env;
    Exchange miss(env, {response("encoded", "Cache-Control: max-age=60\r\nContent-Encoding: gzip\r\n")});
    miss.until_head();
    env.steady.advance(5s); // receiving/draining the body counts toward age
    miss.finish();
    CHECK(miss.output == "encoded" && env.entry());
    env.steady.advance(3s); env.wall.advance(24h);
    Exchange hit(env, {}, "Cache-Control: only-if-cached\r\n"); hit.finish();
    CHECK(hit.output == "encoded" && hit.resolves == 0 && hit.connections == 0);
    CHECK(hit.heads[0].response.headers.get("Age") == "8");
    CHECK(hit.heads[0].response.headers.get("Content-Encoding") == "gzip");
    CHECK(hit.heads[0].response.headers.get_all("Via").size() == 1);
    CHECK(hit.heads[0].length == 7 && hit.heads[0].framing == http::BodyFraming::fixed_length);
    for (const auto extra : {"Authorization: x\r\n", "Cookie: a=b\r\n", "Range: bytes=0-1\r\n",
            "If-None-Match: \"v1\"\r\n", "Cache-Control: no-store\r\n", "Cache-Control: no-cache\r\n"}) {
        Exchange forbidden(env, {}, std::string(extra) + "Cache-Control: only-if-cached\r\n"); forbidden.finish();
        CHECK(forbidden.failure == 504 && forbidden.resolves == 0 && forbidden.connections == 0);
    }
    Exchange head(env, {}, "Cache-Control: only-if-cached\r\n", "/item", http::Method::HEAD); head.finish();
    Exchange absent(env, {}, "Cache-Control: only-if-cached\r\n", "/missing"); absent.finish();
    env.steady.advance(60s);
    Exchange stale(env, {}, "Cache-Control: only-if-cached\r\n"); stale.finish();
    CHECK(head.failure == 504 && absent.failure == 504 && stale.failure == 504);
    CHECK(head.resolves + absent.resolves + stale.resolves == 0);
    CHECK(env.stats->count(CacheEvent::hit) == 1 && env.stats->count(CacheEvent::miss) == 1);
}

void request_and_response_bypass() {
    for (const auto extra : {"Authorization: x\r\n", "Cookie: a=b\r\n", "Range: bytes=0-1\r\n",
            "If-None-Match: \"v1\"\r\n", "Cache-Control: no-store\r\n"}) {
        Environment env; fill(env);
        const auto version = env.entry()->version;
        Exchange bypass(env, {response("private")}, extra); bypass.finish();
        CHECK(bypass.output == "private" && bypass.resolves == 1 && env.entry()->version == version);
        CHECK(env.stats->count(CacheEvent::bypass) == 1);
    }
    for (const auto extra : {"Cache-Control: private\r\n", "Cache-Control: no-store\r\n",
            "Cache-Control: max-age=60\r\nVary: Accept-Encoding\r\n", "Set-Cookie: a=b\r\n",
            "Content-Range: bytes 0-2/3\r\n", "Cache-Control: max-age=broken\r\n"}) {
        Environment env; fill(env, response("abc", extra)); CHECK(!env.entry());
    }
    for (const auto field : {"Age", "Cache-Control", "Content-Encoding"}) {
        Environment nominated;
        fill(nominated, response("abc", std::string("Cache-Control: no-cache\r\nAge: 100\r\nETag: \"v1\"\r\nConnection: ") + field + "\r\n"));
        CHECK(!nominated.entry());
    }
    Environment env;
    Exchange head(env, {response()}, {}, "/item", http::Method::HEAD); head.finish();
    CHECK(!head.failure && head.output.empty() && !env.entry());
    Exchange conditional(env, {validation()}, "If-None-Match: \"v1\"\r\n"); conditional.finish();
    CHECK(conditional.heads[0].response.status_code == 304 && !conditional.failure);
    Exchange invalid(env); invalid.transaction.reset();
    auto request = invalid.request; request.target = "/origin-form";
    auto rejected = test::take(env.factory->create(request, {{{2, 1}, 1}, 256, env.steady, env.wall}));
    CHECK(std::get<http::TransactionFailure>(rejected->on_event(http::StartTransaction{})).status == http::StatusCode::BadRequest);
}

void cached_output_uses_downstream_window() {
    constexpr std::size_t window = 64 * 1024;
    Environment env;
    env.steady.set(core::real_steady_clock().now());
    // Include binary bytes and a final short segment to detect lost, repeated,
    // or reordered bytes across both application segments and socket writes.
    std::string body(2 * window + 13, '\0');
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>(i % 251);
    Exchange miss(env, {response(body)});
    miss.read_window = 16384; miss.body_window = 16384; miss.finish();
    CHECK(!miss.failure && miss.output == body && env.entry());

    for (const std::size_t output_window : {std::size_t{8192}, window}) {
        for (const bool slow : {false, true}) {
            auto config = session(env); config.max_output_bytes = output_window;
            http::HttpSession connection(config, {2, 1}, core::real_steady_clock(), env.wall);
            proxy_test::Channel client(proxy_test::get("http://127.0.0.1:8080/item", "GET", true,
                "HTTP/1.1", "Cache-Control: only-if-cached\r\n"), slow ? 997 : output_window, slow ? 3 : 0);
            concurrency::SingleSessionDriver driver;
            const auto result = driver.drive_channel(client, connection);
            CHECK(result.completed_cleanly && connection.stats().requests_served == 1);
            CHECK(connection.peak_output_bytes() == output_window);
            const auto parsed = proxy_test::parse_all(client.output, {http::Method::GET});
            CHECK(parsed[0].complete && parsed[0].response.status_code == 200 && parsed[0].response.body == body);
            CHECK(parsed[0].consumed == client.output.size());
            if (slow) {
                CHECK(result.short_write_count > 0 && result.would_block_write_count > 0);
            } else {
                // One head, then full output windows and a final short body.
                // This fails if upstream read sizing fragments cache hits.
                CHECK(client.writes == 1 + (body.size() + output_window - 1) / output_window);
            }
            CHECK(env.storage->statistics().readers == 0);
        }
    }
    CHECK(env.forwarding->snapshot().origin_connections == 1);
    CHECK(env.stats->count(CacheEvent::hit) == 4);

    auto request = miss.request;
    auto hit = test::take(env.factory->create(request, {{{3, 1}, 1}, window, env.steady, env.wall}));
    CHECK(std::holds_alternative<http::ResponseHead>(hit->on_event(http::StartTransaction{})));
    std::string output;
    for (const std::size_t capacity : {2 * window, std::size_t{1}, std::size_t{8192}, window}) {
        auto step = hit->on_event(http::OutputDrained{capacity});
        const auto* part = std::get_if<http::ResponseBody>(&step);
        CHECK(part && part->bytes && !part->bytes->empty());
        CHECK(part->bytes->size() <= capacity && part->bytes->size() <= window);
        output += text(*part->bytes);
    }
    CHECK(output == body);
    CHECK(std::holds_alternative<http::ResponseFinished>(hit->on_event(http::OutputDrained{window})));
    CHECK(env.storage->statistics().readers == 0);
}

void limits_and_incomplete_capture() {
    auto limits = cache::storage::Limits{}; limits.object_bytes = 2;
    Environment small(limits); fill(small); CHECK(!small.entry());
    CHECK(small.storage->statistics().candidate_bytes == 0);
    proxy::CacheProxyConfig config; config.policy.object_bytes = 2;
    Environment policy_limit({}, config); fill(policy_limit); CHECK(!policy_limit.entry());
    limits.entries = 1; limits.object_bytes = 4000; limits.resident_bytes = 4000; limits.total_bytes = 5000; limits.metadata_bytes = 5000;
    Environment tight(limits);
    Exchange constrained(tight, {response(std::string(3800, 'x'))}); constrained.until_head();
    CHECK(tight.storage->statistics().candidate_bytes > 0);
    constrained.finish(); CHECK(!constrained.failure && constrained.output.size() == 3800 && !tight.entry());
    CHECK(tight.storage->statistics().total_bytes <= limits.total_bytes && tight.storage->statistics().candidate_bytes == 0);
    for (const auto wire : {
            "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nETag: \"v1\"\r\n\r\nabc",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nETag: \"v1\"\r\n\r\n3\r\nabc\r\n",
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nETag: \"v1\"\r\n\r\n3\r\nabc\r\n0\r\nCache-Control: max-age=60\r\n\r\n"}) {
        Environment env; Exchange broken(env, {wire}); broken.finish();
        CHECK(broken.failure == 502 && !env.entry() && env.storage->statistics().candidate_bytes == 0);
    }
    Environment close; fill(close, "HTTP/1.1 200 OK\r\nETag: \"v1\"\r\n\r\nabc"); CHECK(!close.entry());
    Environment cancelled;
    { Exchange interrupted(cancelled, {response()}); interrupted.until_head(); CHECK(cancelled.storage->statistics().candidate_bytes > 0); }
    CHECK(!cancelled.entry() && cancelled.storage->statistics().candidate_bytes == 0);
    CHECK(cancelled.stats->count(CacheEvent::cancelled) == 1);
    Environment empty; fill(empty, response("")); CHECK(empty.entry() && empty.entry()->body->bytes().empty());
    Exchange empty_hit(empty); empty_hit.finish(); CHECK(!empty_hit.failure && empty_hit.resolves == 0);

    limits = {}; limits.readers = 1;
    Environment readers(limits); fill(readers);
    auto pin = readers.entry();
    Exchange exhausted(readers, {}, "Cache-Control: only-if-cached\r\n"); exhausted.finish();
    CHECK(exhausted.failure == 504 && exhausted.resolves == 0 && exhausted.connections == 0);
    pin.reset();
    Exchange available(readers); available.finish(); CHECK(available.output == "old");
}

void validators_and_changes() {
    for (const auto tag : {"\"v1\"", "W/\"v1\""}) {
        Environment env; fill(env, response("old", std::string("Cache-Control: no-cache\r\nETag: ") + tag + "\r\n"));
        auto old = env.entry();
        Exchange validate(env, {validation(std::string("ETag: ") + tag + "\r\nCache-Control: max-age=120\r\n")}); validate.finish();
        CHECK(!validate.failure && validate.output == "old" && validate.heads[0].response.status_code == 200);
        CHECK(validate.requests[0].find(std::string("If-None-Match: ") + tag) != std::string::npos);
        CHECK(env.entry()->body == old->body && env.entry()->version != old->version && validate.closes == 1);
        Exchange hit(env); hit.finish(); CHECK(hit.resolves == 0 && hit.output == "old");
        CHECK(env.stats->count(CacheEvent::validation_request) == 1 && env.stats->count(CacheEvent::validation_304) == 1);
    }
    Environment modified;
    const std::string lm = "Last-Modified: Wed, 31 Dec 2025 23:00:00 GMT\r\n";
    fill(modified, response("old", "Cache-Control: no-cache\r\n" + lm));
    Exchange check(modified, {validation(lm + "Cache-Control: max-age=60\r\n")}); check.finish();
    CHECK(check.output == "old" && check.requests[0].find("If-Modified-Since: Wed, 31 Dec 2025") != std::string::npos);
    for (const auto fields : {"Cache-Control: max-age=60\r\nETag: \"v2\"\r\n", "Cache-Control: no-store\r\n",
            "Cache-Control: private\r\n", "Vary: Accept-Encoding\r\n"}) {
        Environment env; fill(env); env.steady.advance(61s);
        Exchange changed(env, {response("new", fields)}); changed.finish();
        CHECK(changed.output == "new" && changed.resolves == 1);
        if (std::string_view(fields).find("v2") != std::string_view::npos) CHECK(text(env.entry()->body->bytes()) == "new");
        else CHECK(!env.entry());
    }
    Environment oversize; fill(oversize); oversize.steady.advance(61s);
    // The replacement exceeds the per-object limit; old version must go.
    auto huge = response(std::string(8 * 1024 * 1024 + 1, 'z'));
    Exchange large(oversize, {std::move(huge)}); large.read_window = 16384; large.body_window = 16384; large.finish();
    CHECK(!large.failure && !oversize.entry());
}

void unusable_validation_and_failure() {
    for (const auto fields : {"ETag: \"other\"\r\n", "ETag: \"v1\"\r\nContent-Encoding: gzip\r\n",
            "ETag: \"v1\"\r\nContent-Length: 123\r\n", "Cache-Control: private\r\nETag: \"v1\"\r\n"}) {
        Environment env; fill(env); env.steady.advance(61s);
        Exchange retry(env, {validation(fields), response("new")}); retry.finish();
        CHECK(retry.output == "new" && retry.resolves == 2 && retry.closes == 2);
        CHECK(retry.requests[0].find("If-None-Match") != std::string::npos && retry.requests[1].find("If-None-Match") == std::string::npos);
    }
    Environment repeated; fill(repeated); repeated.steady.advance(61s);
    Exchange bad(repeated, {validation("ETag: \"wrong\"\r\n"), validation()}); bad.finish();
    CHECK(bad.failure == 502 && bad.output.empty() && bad.heads.empty() && bad.resolves == 2);
    Environment unexpected; Exchange wrong(unexpected, {validation()}); wrong.finish(); CHECK(wrong.failure == 502);
    Environment failed; fill(failed); failed.steady.advance(61s);
    Exchange timeout(failed);
    WorkEvent event; event.status = WorkStatus::timed_out;
    timeout.step = timeout.transaction->on_event(event); timeout.finish();
    CHECK(timeout.failure == 504 && timeout.output.empty() && failed.entry());
    Exchange broken(failed, {response("new").substr(0, response("new").size() - 1)}); broken.finish();
    CHECK(broken.failure == 502 && !failed.entry());
    Environment gone; fill(gone);
    Exchange removed(gone, {"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"}, "Cache-Control: no-cache\r\n"); removed.finish();
    CHECK(!removed.failure && removed.heads[0].response.status_code == 404 && !gone.entry());
}

void real_origin_streaming_and_commitment() {
    auto runtime = test::take(net::initialize_network());
    const auto now = http::format_http_date(core::real_system_clock().now());
    const auto wire = "HTTP/1.1 200 OK\r\nDate: " + now + "\r\nCache-Control: max-age=600\r\nContent-Encoding: gzip\r\n"
        "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\nX-Checksum: 123\r\n\r\n";
    proxy_test::MultiOrigin origin({{{wire}}}, 3);
    Environment env;
    proxy_test::Channel channel(proxy_test::get(origin.url(), "GET", false) + proxy_test::get(origin.url()), 5, 3);
    const auto result = http::run_http_session(channel, session(env)); origin.join();
    CHECK(result.completed_cleanly && result.requests_served == 2 && result.peak_output_bytes <= 256);
    const auto parsed = proxy_test::parse_all(channel.output, {http::Method::GET, http::Method::GET});
    CHECK(parsed[0].complete && parsed[1].complete && parsed[0].response.body == "abcdef" && parsed[1].response.body == "abcdef");
    CHECK(parsed[1].response.headers.get("Content-Length") == "6" && parsed[1].response.headers.get("Content-Encoding") == "gzip");
    CHECK(origin.connections() == 1 && env.forwarding->snapshot().origin_connections == 1 && env.stats->count(CacheEvent::hit) == 1);

    proxy_test::MultiOrigin truncated({{{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nETag: \"x\"\r\n\r\n3\r\nabc\r\n"}}});
    Environment broken;
    proxy_test::Channel client(proxy_test::get(truncated.url()), 2);
    const auto failed = http::run_http_session(client, session(broken)); truncated.join();
    CHECK(!failed.completed_cleanly && client.output.find("200 OK") != std::string::npos);
    CHECK(client.output.find("502") == std::string::npos && !client.output.ends_with("0\r\n\r\n"));
    CHECK(broken.storage->statistics().entries == 0 && broken.storage->statistics().candidate_bytes == 0);

    const auto first = "HTTP/1.1 200 OK\r\nDate: " + now + "\r\nCache-Control: no-cache\r\nETag: W/\"same\"\r\nContent-Length: 3\r\n\r\nold";
    const auto not_modified = "HTTP/1.1 304 Not Modified\r\nDate: " + now + "\r\nCache-Control: max-age=600\r\nETag: W/\"same\"\r\n\r\n";
    proxy_test::MultiOrigin validator({{{first}}, {{not_modified}}}, 2);
    Environment refreshed;
    proxy_test::Channel pipeline(proxy_test::get(validator.url(), "GET", false) + proxy_test::get(validator.url(), "GET", false) + proxy_test::get(validator.url()), 3, 4);
    CHECK(http::run_http_session(pipeline, session(refreshed)).completed_cleanly); validator.join();
    const auto answers = proxy_test::parse_all(pipeline.output, {http::Method::GET, http::Method::GET, http::Method::GET});
    for (const auto& answer : answers) CHECK(answer.complete && answer.response.status_code == 200 && answer.response.body == "old");
    CHECK(validator.connections() == 2 && validator.requests()[1].find("If-None-Match: W/\"same\"") != std::string::npos);
    CHECK(refreshed.stats->count(CacheEvent::validation_304) == 1 && refreshed.stats->count(CacheEvent::hit) == 1);

    proxy_test::MultiOrigin cancelled_origin({{{response(std::string(10000, 'c'))}}});
    Environment cancelled;
    proxy_test::Channel disconnected(proxy_test::get(cancelled_origin.url()), 5); disconnected.fail_after = 220;
    CHECK(!http::run_http_session(disconnected, session(cancelled)).completed_cleanly); cancelled_origin.join();
    CHECK(disconnected.output.find("200 OK") != std::string::npos);
    CHECK(cancelled.storage->statistics().entries == 0 && cancelled.storage->statistics().candidate_bytes == 0);
    CHECK(cancelled.stats->count(CacheEvent::cancelled) == 1);
}
} // namespace

int main() {
    miss_hit_age_and_cache_only(); request_and_response_bypass(); limits_and_incomplete_capture();
    cached_output_uses_downstream_window();
    validators_and_changes(); unusable_validation_and_failure(); real_origin_streaming_and_commitment();
    std::cout << "cache proxy transaction acceptance passed\n";
}
