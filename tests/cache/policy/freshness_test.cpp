#include "fixtures.hpp"

int main() {
    using namespace cache;
    using namespace policy_test;
    policy::CachePolicy policy;
    auto req = request(); auto snapshot = entry();
    core::ManualSteadyClock steady;
    core::ManualSystemClock system(wall());
    auto evaluate = [&] { return policy.evaluate(req, snapshot, steady.now(), system.now()); };
    CHECK(evaluate().kind == DecisionKind::hit && evaluate().current_age == 0s);
    steady.advance(59s);
    CHECK(evaluate().kind == DecisionKind::hit && evaluate().current_age == 59s);
    steady.advance(1s);
    CHECK(evaluate().kind == DecisionKind::revalidate && evaluate().current_age == 60s);
    system.advance(-100000s);
    CHECK(evaluate().current_age == 60s);
    system.advance(200000s);
    CHECK(evaluate().current_age == 60s);
    req.headers.add("Cache-Control", "max-stale=10000");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    req.headers.clear(); req.headers.add("Host", "example.com");
    steady.set(core::SteadyTimePoint{} + 10s);
    req.headers.add("Cache-Control", "max-age=9");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    req.headers.set("Cache-Control", "max-age=10");
    CHECK(evaluate().kind == DecisionKind::hit);
    req.headers.set("Cache-Control", "min-fresh=50");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    req.headers.set("Cache-Control", "min-fresh=49");
    CHECK(evaluate().kind == DecisionKind::hit);
    req.headers.remove("Cache-Control");
    snapshot->headers.set("Cache-Control", "s-maxage=5, max-age=1000");
    CHECK(evaluate().freshness_lifetime == 5s && evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.set("Cache-Control", "no-cache=\"X-Secret\", max-age=1000");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.set("Cache-Control", "max-age=1000");
    req.headers.add("Cache-Control", "no-cache");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    req.headers.remove("Cache-Control");
    snapshot->headers.set("Age", "20");
    snapshot->timing.request_sent = core::SteadyTimePoint{};
    snapshot->timing.final_head_received = core::SteadyTimePoint{} + 5s;
    snapshot->timing.final_head_wall = wall() + 5s;
    CHECK(evaluate().current_age == 30s); // Age 20 + transit 5 + resident 5
    snapshot->headers.set("Date", http::format_http_date(wall() - 100s));
    CHECK(evaluate().current_age == 110s); // apparent 105 dominates
    snapshot->headers.set("Date", http::format_http_date(wall() + 100s));
    CHECK(evaluate().current_age == 30s); // future Date never creates negative age
    snapshot->headers.set("Age", "bad");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.set("Age", "0"); snapshot->headers.add("Age", "0");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.remove("Age");
    snapshot->headers.set("Date", "invalid");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.remove("Date");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.set("Date", http::format_http_date(wall()));
    snapshot->headers.remove("Cache-Control");
    snapshot->headers.set("Expires", http::format_http_date(wall() + 60s));
    CHECK(evaluate().freshness_lifetime == 60s && evaluate().kind == DecisionKind::hit);
    snapshot->headers.set("Expires", http::format_http_date(wall() - 60s));
    CHECK(evaluate().freshness_lifetime == 0s);
    snapshot->headers.set("Expires", "invalid");
    CHECK(evaluate().freshness_lifetime == 0s);
    snapshot->headers.remove("Expires");
    CHECK(evaluate().kind == DecisionKind::revalidate);
    snapshot->headers.remove("ETag");
    CHECK(evaluate().kind == DecisionKind::fetch);
    snapshot->headers.add("Last-Modified", http::format_http_date(wall() - 60s));
    CHECK(evaluate().conditional_headers.contains("If-Modified-Since"));
    snapshot->headers.add("ETag", "W/\"v1\"");
    CHECK(evaluate().conditional_headers.get("If-None-Match") == "W/\"v1\"");
    CHECK(!evaluate().conditional_headers.contains("If-Modified-Since"));
    snapshot->headers.set("Cache-Control", "max-age=2147483648");
    snapshot->timing.request_sent = core::SteadyTimePoint::min();
    snapshot->timing.final_head_received = core::SteadyTimePoint::max();
    steady.set(core::SteadyTimePoint::max());
    CHECK(evaluate().current_age == std::chrono::seconds(policy::maximum_age));
    CHECK(evaluate().kind == DecisionKind::revalidate);
    CHECK(policy::age_header(-1s) == "0");
    CHECK(policy::age_header(std::chrono::seconds::max()) == "2147483648");
    snapshot = entry();
    steady.set(core::SteadyTimePoint{} + 59500ms);
    CHECK(evaluate().kind == DecisionKind::revalidate); // conservative whole-second Age
}
