#include "test_support.hpp"
using namespace std::chrono_literals;

#if defined(MILESTONE06_AVAILABLE)
#include "milestone06_adapter.hpp"
#include <array>
#include <string>
int main() {
    const std::array<std::string, 3> valid{
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "HEAD /a?b=c HTTP/1.1\r\nhOsT: example.test:8080\r\nX-Test: a\r\nX-Test: b\r\n\r\n",
        "GET / HTTP/1.0\r\n\r\n"};
    for (const auto& request : valid) {
        for (std::size_t split = 0; split <= request.size(); ++split) {
            tests::ParserHarness parser;
            auto first = parser.consume(std::string_view(request).substr(0, split));
            CHECK(!first.error);
            auto second = parser.consume(std::string_view(request).substr(split));
            CHECK(!second.error && parser.complete());
            CHECK(first.consumed + second.consumed == request.size());
            CHECK(parser.target() == (request.starts_with("HEAD") ? "/a?b=c" : "/"));
            if (request.starts_with("HEAD")) CHECK(parser.headers("x-test").size() == 2);
            parser.reset();
            CHECK(!parser.complete());
        }
        tests::ParserHarness bytewise;
        for (char byte : request) CHECK(!bytewise.consume(std::string_view(&byte, 1)).error);
        CHECK(bytewise.complete());
    }
    tests::ParserHarness pipeline;
    auto combined = valid[0] + valid[1];
    auto first = pipeline.consume(combined);
    CHECK(!first.error && first.consumed == valid[0].size());
    pipeline.reset();
    auto second = pipeline.consume(std::string_view(combined).substr(first.consumed));
    CHECK(!second.error && second.consumed == valid[1].size());
    for (const std::string malformed : {
        "GET / HTTP/1.1\r\n\r\n", // missing Host
        "GET / HTTP/1.1\r\nHost:\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a,b\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : a\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nBad Name: x\r\n\r\n",
        "GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n",
        "GET / HTTP/2.0\r\nHost: a\r\n\r\n",
        "GET / HTTP/1.1\nHost: a\n\n"}) {
        tests::ParserHarness parser;
        CHECK(parser.consume(malformed).error);
    }
    CHECK(tests::rejects_target_limit(32, std::string(33, 'a')));
    CHECK(tests::rejects_header_byte_limit(64));
    CHECK(tests::rejects_header_count_limit(2));
    CHECK(tests::rejects_connection_input_limit(128));
    CHECK(tests::maps_syntax_to_status(400));
    CHECK(tests::maps_version_to_status(505));
    CHECK(tests::maps_header_limit_to_status(431));
}
#else
int main() { return test::pending(6); }
#endif
