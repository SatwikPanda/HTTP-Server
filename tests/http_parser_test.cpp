#include "http/http_session.hpp"
#include "http/request_parser.hpp"
#include "test_support.hpp"

#include <array>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using http::ParseError;
using http::ParseEvent;
using namespace std::string_literals;

struct Outcome {
    ParseEvent event{ParseEvent::need_more};
    ParseError error{ParseError::none};
    std::size_t consumed{0};
};

Outcome parse(std::string_view input, http::RequestLimits limits = {}) {
    http::RequestParser parser(limits);
    const auto result = parser.consume(input);
    return {result.event, parser.error(), result.consumed};
}

// Delivers `input` in three pieces split at every pair of positions. Like a
// session, each call offers everything received but not yet consumed.
void check_every_split(std::string_view input, std::size_t head_size) {
    for (std::size_t first = 0; first <= input.size(); ++first) {
        for (std::size_t second = first; second <= input.size(); ++second) {
            http::RequestParser parser;
            std::size_t consumed = 0;
            for (std::size_t received : {first, second, input.size()}) {
                const auto result = parser.consume(input.substr(consumed, received - consumed));
                CHECK(result.event != ParseEvent::error);
                consumed += result.consumed;
            }
            CHECK(parser.complete());
            CHECK(consumed == head_size);
            CHECK(parser.head_bytes() == head_size);
        }
    }
}

void test_parsed_fields() {
    std::cout << "[RUN] test_parsed_fields" << std::endl;
    http::RequestParser parser;
    const std::string request =
        "\r\nPURGE /a/b?c=d%20e HTTP/1.0\r\n"
        "Accept:  text/html,\t*/*  \r\n"
        "accept: application/json\r\n"
        "X-Empty:\r\n"
        "X-Obs-Text: caf\xc3\xa9\r\n"
        "\r\n";
    const auto result = parser.consume(request);
    CHECK(result.event == ParseEvent::complete && result.consumed == request.size());

    const auto& parsed = parser.request();
    CHECK(parsed.method == http::Method::UNKNOWN); // kept for the application to reject
    CHECK(parsed.raw_method == "PURGE");
    CHECK(parsed.target == "/a/b?c=d%20e");
    CHECK(parsed.version == http::HttpVersion::http_1_0());
    const auto accept = parsed.headers.get_all("ACCEPT");
    CHECK(accept.size() == 2 && accept[0] == "text/html,\t*/*" && accept[1] == "application/json");
    CHECK(parsed.headers.entries()[0].name == "Accept"); // original case kept
    CHECK(parsed.header("x-empty").value() == "");
    CHECK(parsed.header("x-obs-text").value() == "caf\xc3\xa9");
    CHECK(!parsed.headers.contains("Host")); // HTTP/1.0 does not require it

    // A complete or failed parser takes nothing more until reset().
    CHECK(parser.consume("GET / HTTP/1.1\r\n").consumed == 0);
    parser.reset();
    CHECK(!parser.complete() && parser.head_bytes() == 0 && parser.request().headers.empty());
    CHECK(parser.consume("GET /next HTTP/1.1\r\nHost: h\r\n\r\n").event == ParseEvent::complete);
    CHECK(parser.request().target == "/next" && parser.request().method == http::Method::GET);
    std::cout << "[PASS] test_parsed_fields" << std::endl;
}

void test_three_way_splits() {
    std::cout << "[RUN] test_three_way_splits" << std::endl;
    const std::string head = "HEAD /x HTTP/1.1\r\nHost: [::1]:8080\r\nA: 1\r\nA: 2\r\n\r\n";
    check_every_split(head, head.size());
    // With a second request behind it, the boundary must not move.
    check_every_split(head + "GET / HTTP/1.1\r\nHost: h\r\n\r\n", head.size());
    std::cout << "[PASS] test_three_way_splits" << std::endl;
}

void test_pipelined_boundaries_at_every_split() {
    std::cout << "[RUN] test_pipelined_boundaries_at_every_split" << std::endl;
    const std::array<std::string, 3> heads{
        "GET /1 HTTP/1.1\r\nHost: a\r\n\r\n",
        "\r\nHEAD /2 HTTP/1.1\r\nHost: b\r\nX: y\r\n\r\n",
        "GET /3 HTTP/1.0\r\n\r\n"};
    const std::string stream = heads[0] + heads[1] + heads[2] + "GET /4";

    for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
        http::RequestParser parser;
        std::string buffered; // what a session keeps between reads
        std::vector<std::string> targets;
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
            buffered += stream.substr(offset, chunk);
            for (;;) {
                const auto result = parser.consume(buffered);
                buffered.erase(0, result.consumed);
                CHECK(result.event != ParseEvent::error);
                if (result.event != ParseEvent::complete) break;
                targets.push_back(parser.request().target);
                parser.reset();
            }
        }
        CHECK((targets == std::vector<std::string>{"/1", "/2", "/3"}));
        CHECK(parser.head_bytes() == 6 && buffered.empty()); // "GET /4" is still in progress
    }
    std::cout << "[PASS] test_pipelined_boundaries_at_every_split" << std::endl;
}

void test_malformed_heads_map_to_errors() {
    std::cout << "[RUN] test_malformed_heads_map_to_errors" << std::endl;
    const std::vector<std::pair<std::string, ParseError>> cases{
        {"GET / HTTP/1.1\r\n\r\n", ParseError::missing_host},
        {"GET / HTTP/1.1\r\nHost:\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost:   \r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a\r\nhost: a\r\n\r\n", ParseError::duplicate_host},
        {"GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", ParseError::duplicate_host},
        {"GET / HTTP/1.1\r\nHost: a,b\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a b\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a:\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a:65536\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a:8x\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: [::1\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: []\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a%zz\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: user@a\r\n\r\n", ParseError::invalid_host},
        {"GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", ParseError::obsolete_line_folding},
        {"GET / HTTP/1.1\r\nHost: a\r\n\tfolded\r\n\r\n", ParseError::obsolete_line_folding},
        {"GET / HTTP/1.1\r\n Host: a\r\n\r\n", ParseError::obsolete_line_folding},
        {"GET / HTTP/1.1\r\nHost : a\r\n\r\n", ParseError::malformed_header},
        {"GET / HTTP/1.1\r\nHost: a\r\nBad Name: x\r\n\r\n", ParseError::malformed_header},
        {"GET / HTTP/1.1\r\nHost: a\r\n: x\r\n\r\n", ParseError::malformed_header},
        {"GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", ParseError::malformed_header},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n"s, ParseError::malformed_header},
        {"GET / HTTP/1.1\r\nHost: a\r\nX: a\x7f\r\n\r\n", ParseError::malformed_header},
        {"GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n", ParseError::malformed_request_line},
        {"GET  / HTTP/1.1\r\nHost: a\r\n\r\n", ParseError::malformed_request_line},
        {" GET / HTTP/1.1\r\nHost: a\r\n\r\n", ParseError::malformed_request_line},
        {"GET / HTTP/1.1 \r\nHost: a\r\n\r\n", ParseError::malformed_request_line},
        {"GET /\r\nHost: a\r\n\r\n", ParseError::malformed_request_line},
        {"GET\r\n\r\n", ParseError::malformed_request_line},
        {"G(T / HTTP/1.1\r\nHost: a\r\n\r\n", ParseError::invalid_method},
        {"GET\t/ HTTP/1.1\r\nHost: a\r\n\r\n", ParseError::invalid_method},
        {"GET /a#frag HTTP/1.1\r\nHost: a\r\n\r\n", ParseError::invalid_target},
        {"GET /\xc3\xa9 HTTP/1.1\r\nHost: a\r\n\r\n", ParseError::invalid_target},
        {"GET / http/1.1\r\nHost: a\r\n\r\n", ParseError::malformed_version},
        {"GET / HTTP/1.10\r\nHost: a\r\n\r\n", ParseError::malformed_version},
        {"GET / HTTP/11\r\nHost: a\r\n\r\n", ParseError::malformed_version},
        {"GET / HTTP/2.0\r\nHost: a\r\n\r\n", ParseError::unsupported_version},
        {"GET / HTTP/1.2\r\nHost: a\r\n\r\n", ParseError::unsupported_version},
        {"GET / HTTP/0.9\r\nHost: a\r\n\r\n", ParseError::unsupported_version},
        {"GET / HTTP/1.1\nHost: a\n\n", ParseError::bare_line_ending},
        {"GET / HTTP/1.1\r\nHost: a\n\r\n", ParseError::bare_line_ending},
        {"GET / HTTP/1.1\r\nHost: a\r\r\n\r\n", ParseError::bare_line_ending},
        {"GET / HTTP/1.1\r\nHost: a\rX\r\n\r\n", ParseError::bare_line_ending},
        {"\n", ParseError::bare_line_ending},
    };
    for (const auto& [input, expected] : cases) {
        const auto outcome = parse(input);
        if (outcome.event != ParseEvent::error || outcome.error != expected) {
            std::cerr << "unexpected outcome for: " << input << " -> " << http::to_string(outcome.error) << "\n";
        }
        CHECK(outcome.event == ParseEvent::error);
        CHECK(outcome.error == expected);
        CHECK(http::status_for(outcome.error) == (expected == ParseError::unsupported_version
            ? http::StatusCode::HttpVersionNotSupported : http::StatusCode::BadRequest));
    }

    // Accepted host forms.
    for (std::string_view host : {"localhost", "example.test:8080", "127.0.0.1:0", "[::1]", "[::1]:443",
                                  "[2001:db8::ffff:192.0.2.1]", "my_host.local", "a%41b", "x:65535"}) {
        const auto outcome = parse("GET / HTTP/1.1\r\nHost: " + std::string(host) + "\r\n\r\n");
        if (outcome.event != ParseEvent::complete) std::cerr << "rejected host: " << host << "\n";
        CHECK(outcome.event == ParseEvent::complete);
    }
    std::cout << "[PASS] test_malformed_heads_map_to_errors" << std::endl;
}

void test_errors_are_detected_early() {
    std::cout << "[RUN] test_errors_are_detected_early" << std::endl;
    // Binary garbage fails on its first byte instead of waiting for a line end.
    CHECK(parse(std::string("\x16\x03\x01\x02\x00", 5)).consumed == 1);
    // An overlong target fails at the first byte past the limit.
    http::RequestLimits limits;
    limits.max_target_bytes = 16;
    const auto target = parse("GET /" + std::string(1000, 'a'), limits);
    CHECK(target.event == ParseEvent::error && target.error == ParseError::target_too_long);
    CHECK(target.consumed == 4 + 16 + 1);
    CHECK(http::status_for(target.error) == http::StatusCode::UriTooLong);
    // So does a request line that never ends.
    limits = {};
    limits.max_request_line_bytes = 20;
    const auto line = parse("GET / HTTP/1.1xxxxxxxxxxxxxxxxxxxxxxx", limits);
    CHECK(line.event == ParseEvent::error && line.error == ParseError::malformed_version);
    const auto long_method = parse(std::string(100, 'M'), limits);
    CHECK(long_method.error == ParseError::request_line_too_long && long_method.consumed == 21);
    CHECK(http::status_for(long_method.error) == http::StatusCode::BadRequest);
    std::cout << "[PASS] test_errors_are_detected_early" << std::endl;
}

void test_limits_are_exact() {
    std::cout << "[RUN] test_limits_are_exact" << std::endl;
    const std::string request = "GET /abcd HTTP/1.1\r\nHost: a\r\nX: 1\r\n\r\n";

    http::RequestLimits limits;
    limits.max_header_bytes = request.size();
    CHECK(parse(request, limits).event == ParseEvent::complete);
    limits.max_header_bytes = request.size() - 1;
    auto outcome = parse(request, limits);
    CHECK(outcome.error == ParseError::header_too_large && outcome.consumed == request.size());
    CHECK(http::status_for(outcome.error) == http::StatusCode::HeaderFieldsTooLarge);

    // Ignored leading blank lines still count toward the head.
    limits.max_header_bytes = request.size() + 1;
    CHECK(parse("\r\n" + request, limits).error == ParseError::header_too_large);
    CHECK(parse("\r\n" + request, http::RequestLimits{}).event == ParseEvent::complete);

    limits = {};
    limits.max_target_bytes = 5; // "/abcd"
    CHECK(parse(request, limits).event == ParseEvent::complete);
    limits.max_target_bytes = 4;
    CHECK(parse(request, limits).error == ParseError::target_too_long);

    limits = {};
    limits.max_request_line_bytes = 18; // "GET /abcd HTTP/1.1"
    CHECK(parse(request, limits).event == ParseEvent::complete);
    limits.max_request_line_bytes = 17;
    CHECK(parse(request, limits).error == ParseError::request_line_too_long);

    limits = {};
    limits.max_header_count = 2;
    CHECK(parse(request, limits).event == ParseEvent::complete);
    limits.max_header_count = 1;
    outcome = parse(request, limits);
    CHECK(outcome.error == ParseError::too_many_headers);
    CHECK(http::status_for(outcome.error) == http::StatusCode::HeaderFieldsTooLarge);

    CHECK(http::validate_request_limits({}));
    CHECK(!http::validate_request_limits({0, 1, 1, 1}));
    CHECK(!http::validate_request_limits({1, 1, 1, 0}));
    CHECK(!http::validate_request_limits({1, 1, 16 * 1024 * 1024 + 1, 1}));
    std::cout << "[PASS] test_limits_are_exact" << std::endl;
}

// Session level: the parser replaces the old CRLF scan without losing bytes.
void test_session_keeps_bytes_after_the_head() {
    std::cout << "[RUN] test_session_keeps_bytes_after_the_head" << std::endl;
    using namespace concurrency;
    const std::string first = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string second = "GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string combined = first + second;

    for (std::size_t chunk = 1; chunk <= combined.size(); ++chunk) {
        http::HttpSession session;
        Action action = session.initial_action();
        std::size_t offset = 0;
        while (action.is_read()) {
            const std::size_t count = std::min({chunk, action.as_read()->max_bytes, combined.size() - offset});
            CHECK(count > 0);
            action = session.on_event(ReadDataEvent{std::as_bytes(std::span(combined.data() + offset, count))});
            offset += count;
        }
        CHECK(action.is_write());
        CHECK(session.stats().request_received && session.stats().response_status == 200);
        CHECK(session.parser().request().target == "/");
        // Everything read beyond the first head is still there, in order.
        CHECK(session.unconsumed_input() == std::string_view(combined).substr(first.size(), offset - first.size()));
    }
    std::cout << "[PASS] test_session_keeps_bytes_after_the_head" << std::endl;
}

void test_session_rejections() {
    std::cout << "[RUN] test_session_rejections" << std::endl;
    using namespace concurrency;
    struct Case {
        std::string request;
        int status;
        ParseError error;
    };
    const std::vector<Case> cases{
        {"GET / HTTP/1.1\r\n\r\n", 400, ParseError::missing_host},
        {"GET / HTTP/3.0\r\nHost: a\r\n\r\n", 505, ParseError::unsupported_version},
        {"GET /" + std::string(9000, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n", 414, ParseError::target_too_long},
    };
    for (const auto& item : cases) {
        http::HttpSessionConfig config;
        config.max_header_bytes = 16 * 1024;
        config.max_request_line_bytes = 16 * 1024;
        config.max_input_bytes = 16 * 1024;
        http::HttpSession session(config);
        Action action = session.initial_action();
        std::size_t offset = 0;
        while (action.is_read() && offset < item.request.size()) {
            const std::size_t count = std::min(action.as_read()->max_bytes, item.request.size() - offset);
            action = session.on_event(ReadDataEvent{std::as_bytes(std::span(item.request.data() + offset, count))});
            offset += count;
        }
        CHECK(action.is_write());
        const std::string wire(reinterpret_cast<const char*>(action.as_write()->data.data()), action.as_write()->data.size());
        CHECK(wire.starts_with("HTTP/1.1 " + std::to_string(item.status) + " "));
        CHECK(wire.find("\r\nConnection: close\r\n") != std::string::npos);
        CHECK(session.stats().response_status == item.status && session.stats().parse_error == item.error);
        CHECK(!session.stats().request_received);
        // A rejected stream is never parsed further: the parser stays failed.
        CHECK(session.parser().failed());
        if (item.status == 414) CHECK(offset < item.request.size()); // stopped reading early
    }

    // Invalid limits are configuration errors, caught before any I/O.
    http::HttpSessionConfig invalid;
    invalid.max_header_count = 0;
    CHECK(!http::validate_http_session_config(invalid));
    invalid = {};
    invalid.max_input_bytes = 3;
    CHECK(!http::validate_http_session_config(invalid));
    std::cout << "[PASS] test_session_rejections" << std::endl;
}

} // namespace

int main() {
    test_parsed_fields();
    test_three_way_splits();
    test_pipelined_boundaries_at_every_split();
    test_malformed_heads_map_to_errors();
    test_errors_are_detected_early();
    test_limits_are_exact();
    test_session_keeps_bytes_after_the_head();
    test_session_rejections();
    std::cout << "All HTTP parser tests PASSED!" << std::endl;
    return 0;
}
