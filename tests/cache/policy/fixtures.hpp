#pragma once

#include "cache/policy/cache_policy.hpp"
#include "http/date.hpp"
#include "test_support.hpp"

namespace policy_test {
using namespace std::chrono_literals;
inline core::SystemTimePoint wall() {
    return test::take(http::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"));
}
inline http::Request request() {
    http::Request result;
    result.target = "http://example.com/data";
    result.headers.add("Host", "example.com");
    return result;
}
inline http::Response response() {
    http::Response result;
    result.body = "abc";
    result.headers.add("Date", http::format_http_date(wall()));
    result.headers.add("Cache-Control", "max-age=60");
    result.headers.add("ETag", "W/\"v1\"");
    result.headers.add("Content-Type", "text/plain");
    return result;
}
inline std::shared_ptr<cache::Entry> entry() {
    auto result = std::make_shared<cache::Entry>();
    result->key = test::take(http::parse_target(request().target));
    result->headers = response().headers;
    result->version = {7};
    result->timing = {core::SteadyTimePoint{}, core::SteadyTimePoint{}, wall()};
    core::MemoryBudget budget(100);
    result->body = test::take(cache::Body::create(std::vector<std::byte>(3, std::byte{'a'}), test::take(budget.reserve(3))));
    return result;
}
} // namespace policy_test
