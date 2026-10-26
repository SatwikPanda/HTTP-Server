#include "fixtures.hpp"

int main() {
    using namespace cache;
    using namespace policy_test;
    policy::CachePolicy policy;
    auto selected = entry();
    selected->headers.add("Age", "55");
    selected->headers.add("X-Removed", "old");
    http::Response response(http::StatusCode::NotModified);
    response.headers.add("ETag", "W/\"v1\"");
    response.headers.add("Date", http::format_http_date(wall() + 60s));
    response.headers.add("Cache-Control", "max-age=120");
    response.headers.add("Connection", "X-Removed");
    response.headers.add("X-Removed", "must-not-copy");
    response.headers.add("X-Repeat", "first"); response.headers.add("X-Repeat", "second");
    response.headers.add("Content-Length", "3");
    const Timing timing{core::SteadyTimePoint{} + 60s, core::SteadyTimePoint{} + 62s, wall() + 62s};
    auto merge = test::take(policy.revalidate(selected, response, timing));
    CHECK(merge.outcome == ValidationOutcome::refresh_metadata && merge.expected == selected->version);
    CHECK(merge.timing.final_head_received == timing.final_head_received);
    CHECK(merge.merged_headers.get("Age") == "0");
    CHECK(merge.merged_headers.get("Content-Type") == "text/plain");
    CHECK(merge.merged_headers.get_all("X-Repeat").size() == 2);
    CHECK(!merge.merged_headers.contains("Connection") && !merge.merged_headers.contains("Content-Length"));
    CHECK(!merge.merged_headers.contains("X-Removed"));
    CHECK(selected->headers.get("Age") == "55");
    auto refreshed = entry(); refreshed->headers = merge.merged_headers; refreshed->timing = merge.timing;
    auto decision = policy.evaluate(request(), refreshed, timing.final_head_received + 1s, wall());
    CHECK(decision.kind == DecisionKind::hit && decision.current_age == 3s && decision.freshness_lifetime == 120s);
    for (const auto tag : {"W/\"different\"", "malformed", "\"v1\""}) {
        auto mismatch = response; mismatch.headers.set("ETag", tag);
        CHECK(test::take(policy.revalidate(selected, mismatch, timing)).outcome == ValidationOutcome::refetch);
    }
    auto absent = response; absent.headers.remove("ETag");
    CHECK(test::take(policy.revalidate(selected, absent, timing)).outcome == ValidationOutcome::refetch);
    for (const auto field : {"Vary", "Set-Cookie", "Content-Range"}) {
        auto changed = response; changed.headers.add(field, "x");
        CHECK(test::take(policy.revalidate(selected, changed, timing)).outcome == ValidationOutcome::reject);
    }
    for (const auto value : {"private", "no-store", "max-age=bad"}) {
        auto changed = response; changed.headers.set("Cache-Control", value);
        CHECK(test::take(policy.revalidate(selected, changed, timing)).outcome == ValidationOutcome::reject);
    }
    for (const auto field : {"Content-Encoding", "Content-Type", "Content-Location"}) {
        auto changed = response; changed.headers.set(field, "different");
        CHECK(test::take(policy.revalidate(selected, changed, timing)).outcome == ValidationOutcome::reject);
    }
    auto invalid = response; invalid.headers.set("Content-Length", "4");
    CHECK(test::take(policy.revalidate(selected, invalid, timing)).outcome == ValidationOutcome::refetch);
    invalid = response; invalid.headers.add("Transfer-Encoding", "chunked");
    CHECK(test::take(policy.revalidate(selected, invalid, timing)).outcome == ValidationOutcome::refetch);
    invalid = response; invalid.headers.set("Connection", "ETag");
    CHECK(test::take(policy.revalidate(selected, invalid, timing)).outcome == ValidationOutcome::refetch);
    invalid = response; invalid.headers.remove("Date");
    CHECK(test::take(policy.revalidate(selected, invalid, timing)).outcome == ValidationOutcome::refetch);
    invalid = response; invalid.body = "x";
    CHECK(test::take(policy.revalidate(selected, invalid, timing)).outcome == ValidationOutcome::refetch);
    invalid = response; invalid.set_status(http::StatusCode::OK);
    CHECK(test::take(policy.revalidate(selected, invalid, timing)).outcome == ValidationOutcome::refetch);
    CHECK(!policy.revalidate({}, response, timing));
    selected->headers.remove("ETag");
    selected->headers.add("Last-Modified", http::format_http_date(wall() - 60s));
    response.headers.remove("ETag");
    response.headers.add("Last-Modified", http::format_http_date(wall() - 60s));
    CHECK(test::take(policy.revalidate(selected, response, timing)).outcome == ValidationOutcome::refresh_metadata);
    response.headers.set("Last-Modified", http::format_http_date(wall() - 61s));
    CHECK(test::take(policy.revalidate(selected, response, timing)).outcome == ValidationOutcome::refetch);
}
