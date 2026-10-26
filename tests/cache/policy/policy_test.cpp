#include "fixtures.hpp"

namespace {
using namespace cache;
using namespace policy_test;
void directives() {
    http::HeaderMap headers;
    headers.add("cache-control", "MAX-AGE = \"6\\0\", extension=\"x,y\\\"z\", No-Cache=\"Set-Cookie, X-Private\"");
    headers.add("CACHE-CONTROL", "s-maxage=30, min-fresh=2, max-stale, only-if-cached, must-revalidate, proxy-revalidate");
    auto control = policy::parse_cache_control(headers);
    CHECK(!control.malformed && control.max_age.value == 60s && control.shared_max_age.value == 30s);
    CHECK(control.no_cache && control.only_if_cached && control.min_fresh.value == 2s);
    CHECK(control.must_revalidate && control.proxy_revalidate);
    headers.add("Cache-Control", "max-age=60");
    CHECK(!policy::parse_cache_control(headers).malformed);
    headers.add("Cache-Control", "max-age=61");
    CHECK(policy::parse_cache_control(headers).malformed);
    for (const auto value : {"max-age=", "max-age=-1", "max-age=+1", "max-age=1.0", "s-maxage=bad, max-age=1000",
            "max-age=2147483649", "max-age=184467440737095516160", "max-age=\"\"", "no-store=1", "only-if-cached=x",
            "foo=\"unterminated", "max-age=1,", ",max-age=1", "max-age=1 max-age=2", "no-cache=\"x\"garbage"}) {
        http::HeaderMap malformed; malformed.add("Cache-Control", value);
        CHECK(policy::parse_cache_control(malformed).malformed);
    }
    http::HeaderMap boundary; boundary.add("Cache-Control", "max-age=2147483648");
    CHECK(!policy::parse_cache_control(boundary).malformed);
    boundary.add("Cache-Control", std::string(65537, 'x'));
    CHECK(policy::parse_cache_control(boundary).malformed);
}
void eligibility_matrix() {
    policy::CachePolicy policy({3});
    auto req = request(); auto res = response();
    CHECK(policy.can_store(req, res, http::BodyFraming::fixed_length, true));
    CHECK(policy.can_store(req, res, http::BodyFraming::chunked, true));
    for (auto framing : {http::BodyFraming::none, http::BodyFraming::close_delimited})
        CHECK(!policy.can_store(req, res, framing, true));
    CHECK(!policy.can_store(req, res, http::BodyFraming::fixed_length, false));
    CHECK(!policy.can_store_stream(req, res, http::BodyFraming::chunked, true, 4));
    res.body.clear();
    CHECK(policy.can_store_stream(req, res, http::BodyFraming::chunked, true, 3));
    for (auto code : {201, 204, 206, 304, 404, 500}) {
        auto other = response(); other.set_status(code);
        CHECK(!policy.can_store(req, other, http::BodyFraming::fixed_length, true));
    }
    for (const auto field : {"Authorization", "Cookie", "Range", "If-Match", "If-None-Match",
            "If-Modified-Since", "If-Unmodified-Since", "If-Range"}) {
        auto restricted = req; restricted.headers.add(field, "x");
        CHECK(!policy.can_store(restricted, res, http::BodyFraming::fixed_length, true));
        const auto decision = policy.evaluate(restricted, entry(), {}, wall());
        CHECK(decision.kind == DecisionKind::bypass && decision.network_allowed && !decision.lookup_allowed && !decision.store_allowed);
        restricted.headers.add("Cache-Control", "only-if-cached");
        auto cache_only = policy.evaluate(restricted, entry(), {}, wall());
        CHECK(cache_only.kind == DecisionKind::cache_only_failure && !cache_only.network_allowed);
    }
    for (const auto method : {http::Method::HEAD, http::Method::POST, http::Method::CONNECT}) {
        auto restricted = req; restricted.method = method;
        CHECK(!policy.can_store(restricted, res, http::BodyFraming::fixed_length, true));
        restricted.headers.add("Cache-Control", "only-if-cached");
        CHECK(!policy.evaluate(restricted, entry(), {}, wall()).network_allowed);
    }
    for (const auto field : {"Set-Cookie", "Vary", "Content-Range"}) {
        auto restricted = response(); restricted.headers.add(field, "");
        CHECK(!policy.can_store(req, restricted, http::BodyFraming::fixed_length, true));
        auto cached = entry(); cached->headers = restricted.headers;
        CHECK(policy.evaluate(req, cached, {}, wall()).kind == DecisionKind::fetch);
    }
    for (const auto control : {"no-store", "private", "private=\"X-Secret\"", "s-maxage=broken, max-age=1000"}) {
        auto restricted = response(); restricted.headers.add("Cache-Control", control);
        CHECK(!policy.can_store(req, restricted, http::BodyFraming::fixed_length, true));
    }
    auto opaque = response(); opaque.headers.add("Content-Encoding", "gzip");
    CHECK(policy.can_store(req, opaque, http::BodyFraming::fixed_length, true));
    opaque.headers.add("Content-Length", "4");
    CHECK(!policy.can_store(req, opaque, http::BodyFraming::fixed_length, true));
    opaque.headers.set("Content-Length", "3");
    CHECK(policy.can_store(req, opaque, http::BodyFraming::fixed_length, true));
    opaque.headers.add("Transfer-Encoding", "chunked");
    CHECK(!policy.can_store(req, opaque, http::BodyFraming::chunked, true));
    auto no_lifetime = response(); no_lifetime.headers.remove("Cache-Control"); no_lifetime.headers.remove("ETag");
    CHECK(!policy.can_store(req, no_lifetime, http::BodyFraming::fixed_length, true));
    no_lifetime.headers.add("Expires", "invalid");
    CHECK(!policy.can_store(req, no_lifetime, http::BodyFraming::fixed_length, true));
    no_lifetime.headers.add("Last-Modified", http::format_http_date(wall() - 60s));
    CHECK(policy.can_store(req, no_lifetime, http::BodyFraming::fixed_length, true));
}
void permissions() {
    policy::CachePolicy policy;
    auto req = request();
    auto missing = policy.evaluate(req, {}, {}, wall());
    CHECK(missing.kind == DecisionKind::fetch && missing.lookup_allowed && missing.store_allowed && missing.network_allowed);
    for (const auto control : {"only-if-cached", "only-if-cached, no-store", "only-if-cached, no-cache",
            "no-store, only-if-cached", "max-age=invalid, only-if-cached", "bad=\"unterminated, only-if-cached"}) {
        auto restricted = req; restricted.headers.add("Cache-Control", control);
        const auto result = policy.evaluate(restricted, {}, {}, wall());
        CHECK(result.kind == DecisionKind::cache_only_failure && !result.network_allowed && !result.store_allowed);
    }
    req.headers.add("Cache-Control", "only-if-cached");
    CHECK(policy.evaluate(req, entry(), {}, wall()).kind == DecisionKind::hit);
    CHECK(policy.evaluate(req, entry(), core::SteadyTimePoint{} + 60s, wall()).kind == DecisionKind::cache_only_failure);
    req.headers.add("Cache-Control", "no-cache");
    CHECK(!policy.evaluate(req, entry(), {}, wall()).network_allowed);
    req.headers.set("Cache-Control", "no-store");
    CHECK(policy.evaluate(req, entry(), {}, wall()).kind == DecisionKind::bypass);
    auto other = entry(); other->key.path_query = "/other";
    CHECK(policy.evaluate(request(), other, {}, wall()).kind == DecisionKind::fetch);
}
}
int main() { directives(); eligibility_matrix(); permissions(); }
