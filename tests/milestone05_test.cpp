#include "test_support.hpp"
using namespace std::chrono_literals;

#if defined(MILESTONE05_AVAILABLE)
#include "milestone05_adapter.hpp"
#include <string>
int main() {
    auto response = tests::fixed_response();
    auto wire = tests::serialize(response);
    CHECK(wire);
    auto boundary = wire.value().find("\r\n\r\n");
    CHECK(boundary != std::string::npos);
    CHECK(wire.value().starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(tests::content_length(response) == wire.value().size() - boundary - 4);
    CHECK(tests::header(response, "date").size() == 29);
    CHECK(tests::header(response, "CONTENT-TYPE").starts_with("text/plain"));
    CHECK(tests::header(response, "connection") == "close");
    tests::add_header(response, "Set-Cookie", "a=1");
    tests::add_header(response, "set-cookie", "b=2");
    CHECK(tests::headers(response, "SET-COOKIE").size() == 2);
    for (const auto& name : {"Bad Name", "", "Bad\r\nName", "Bad:Name"}) {
        auto invalid = tests::fixed_response();
        tests::add_header(invalid, name, "value");
        CHECK(!tests::serialize(invalid));
    }
    for (const auto& value : {std::string("x\r\nInjected: yes"), std::string("x\0y", 3)}) {
        auto invalid = tests::fixed_response();
        tests::add_header(invalid, "X-Test", value);
        CHECK(!tests::serialize(invalid));
    }
    CHECK(tests::fixed_http_roundtrip("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 1) == wire.value());
    CHECK(tests::fixed_http_roundtrip("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 7) == wire.value());
    CHECK(tests::oversized_demo_header_is_rejected());
}
#else
int main() { return test::pending(5); }
#endif
