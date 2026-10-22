#include "test_support.hpp"

#include "server/conditional.hpp"

#include <chrono>

namespace {
using namespace std::chrono;
using server::Conditional;
using server::Representation;

const core::SystemTimePoint now = sys_days{year{2024} / June / 1};
const core::SystemTimePoint base = sys_days{year{2024} / January / 2} + hours{3} + minutes{4} + seconds{5};

Representation rep(std::uint64_t size = 1234, core::SystemTimePoint modified = base + milliseconds{678}) { return {size, modified}; }

Conditional decide(const Representation& representation, std::initializer_list<std::pair<const char*, const char*>> fields) {
    http::HeaderMap headers;
    for (const auto& [name, value] : fields) headers.add(name, value);
    return server::evaluate_conditions(headers, representation, now);
}

void tags_and_dates() {
    const auto first = rep();
    const auto tag = server::make_etag(first);
    CHECK(tag.starts_with("W/\"4d2-") && tag.ends_with('"'));
    CHECK(tag == server::make_etag(rep()));                                  // stable
    CHECK(tag != server::make_etag(rep(1235)));                              // size changes it
    CHECK(tag != server::make_etag(rep(1234, base + milliseconds{679})));    // sub-second precision kept
    CHECK(server::make_last_modified(first) == "Tue, 02 Jan 2024 03:04:05 GMT");
    CHECK(server::make_last_modified(rep(1, base + milliseconds{999})) == server::make_last_modified(first)); // whole seconds
    CHECK(server::make_last_modified(rep(1, sys_days{year{1969} / December / 31} + hours{23})) == "Wed, 31 Dec 1969 23:00:00 GMT");
    CHECK(server::make_etag(rep(0, core::SystemTimePoint{})).starts_with("W/\"0-0\""));
    // Pre-epoch times still format (two's-complement tick count).
    const auto old_tag = server::make_etag(rep(1, sys_days{year{1969} / December / 31}));
    CHECK(old_tag.starts_with("W/\"1-") && old_tag.ends_with('"') && old_tag.size() > 10);
}

void if_none_match() {
    const auto r = rep();
    const std::string tag = server::make_etag(r);
    const std::string strong = tag.substr(2); // same opaque value without W/
    CHECK(decide(r, {}) == Conditional::proceed);
    CHECK(decide(r, {{"If-None-Match", tag.c_str()}}) == Conditional::not_modified);
    CHECK(decide(r, {{"If-None-Match", strong.c_str()}}) == Conditional::not_modified); // weak comparison
    CHECK(decide(r, {{"If-None-Match", "*"}}) == Conditional::not_modified);
    CHECK(decide(r, {{"If-None-Match", "  *  "}}) == Conditional::not_modified);
    const std::string list = "\"a\", W/\"b\", " + tag + ", \"c\"";
    CHECK(decide(r, {{"If-None-Match", list.c_str()}}) == Conditional::not_modified);
    CHECK(decide(r, {{"If-None-Match", "\"a\", \"b\""}}) == Conditional::proceed);
    CHECK(decide(r, {{"If-None-Match", "W/\"4d2-0\""}}) == Conditional::proceed);
    const std::string other = server::make_etag(rep(1234, base + seconds{1}));
    CHECK(decide(r, {{"If-None-Match", other.c_str()}}) == Conditional::proceed);       // changed file
    // Repeated field lines form one list.
    CHECK(decide(r, {{"If-None-Match", "\"a\""}, {"If-None-Match", tag.c_str()}}) == Conditional::not_modified);
    // Opaque commas/backslashes inside quotes do not split or match.
    CHECK(decide(r, {{"If-None-Match", "\"x,y\", \"z\\\""}}) == Conditional::proceed);

    // If-None-Match is evaluated first and alone: a non-matching tag does not
    // fall back to a matching If-Modified-Since.
    const char* later = "Wed, 03 Jan 2024 00:00:00 GMT";
    CHECK(decide(r, {{"If-Modified-Since", later}}) == Conditional::not_modified);
    CHECK(decide(r, {{"If-None-Match", "\"nope\""}, {"If-Modified-Since", later}}) == Conditional::proceed);
    // Conflicting: matching tag wins over a date that would say "modified".
    CHECK(decide(r, {{"If-None-Match", tag.c_str()}, {"If-Modified-Since", "Mon, 01 Jan 2024 00:00:00 GMT"}}) == Conditional::not_modified);
    // Malformed conditions are conservative: no 304, and no If-Modified-Since fallback.
    for (const char* bad : {"", "unquoted", "\"a\" \"b\"", "*, \"a\"", "\"a\",", ",\"a\"", "W/", "\"open", "W/*", "* x"}) {
        CHECK(decide(r, {{"If-None-Match", bad}}) == Conditional::proceed);
        CHECK(decide(r, {{"If-None-Match", bad}, {"If-Modified-Since", later}}) == Conditional::proceed);
    }
}

void if_modified_since() {
    const auto r = rep();
    CHECK(decide(r, {{"If-Modified-Since", "Tue, 02 Jan 2024 03:04:05 GMT"}}) == Conditional::not_modified); // equal, sub-second ignored
    CHECK(decide(r, {{"If-Modified-Since", "Tue, 02 Jan 2024 03:04:06 GMT"}}) == Conditional::not_modified);
    CHECK(decide(r, {{"If-Modified-Since", "Tue, 02 Jan 2024 03:04:04 GMT"}}) == Conditional::proceed);     // modified since
    CHECK(decide(r, {{"If-Modified-Since", "Tuesday, 02-Jan-24 03:04:05 GMT"}}) == Conditional::not_modified); // RFC 850
    CHECK(decide(r, {{"If-Modified-Since", "Tue Jan  2 03:04:05 2024"}}) == Conditional::not_modified);       // asctime
    // Invalid, duplicated or non-date values are ignored (full response).
    for (const char* bad : {"", "yesterday", "Tue, 02 Jan 2024 25:00:00 GMT", "Mon, 02 Jan 2024 03:04:05 GMT", "2024-01-02", "Tue, 02 Jan 2024 03:04:05"}) {
        CHECK(decide(r, {{"If-Modified-Since", bad}}) == Conditional::proceed);
    }
    CHECK(decide(r, {{"If-Modified-Since", "Wed, 03 Jan 2024 00:00:00 GMT"}, {"If-Modified-Since", "Wed, 03 Jan 2024 00:00:00 GMT"}}) == Conditional::proceed);
    // Other validators are outside this service's scope and are ignored.
    CHECK(decide(r, {{"If-Match", "\"zzz\""}, {"If-Unmodified-Since", "Mon, 01 Jan 2001 00:00:00 GMT"}}) == Conditional::proceed);
}
} // namespace

int main() {
    tags_and_dates();
    if_none_match();
    if_modified_since();
    return 0;
}
