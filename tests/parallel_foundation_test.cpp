#include "cache/contracts.hpp"
#include "concurrency/lifecycle.hpp"
#include "core/config.hpp"
#include "http/date.hpp"
#include "http/http_session.hpp"
#include "http/response_parser.hpp"
#include "http/response_writer.hpp"
#include "http/serializer.hpp"
#include "http/target.hpp"
#include "http/validators.hpp"
#include "test_support.hpp"

#include <limits>

namespace {
using namespace std::chrono_literals;
using http::ResponseEventKind;
auto bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }

std::string stream_parse(std::string_view wire, std::size_t input_window, std::size_t body_window,
                         http::Method method = http::Method::GET, unsigned expected_interim = 0, bool expect_trailers = false) {
    http::ResponseParser parser(method);
    std::string body;
    std::size_t offset = 0, available = 0;
    unsigned heads = 0, interim = 0, trailers = 0;
    for (std::size_t step = 0; step < wire.size() * 5 + 20; ++step) {
        if (offset == available) available = std::min(wire.size(), available + input_window);
        const auto input = bytes(wire).subspan(offset, available - offset);
        const auto event = parser.consume_stream(input, body_window);
        CHECK(event.consumed <= input.size()); offset += event.consumed;
        CHECK(parser.response().body.empty());
        if (event.kind == ResponseEventKind::error) CHECK(false);
        if (event.kind == ResponseEventKind::informational_head) ++interim;
        if (event.kind == ResponseEventKind::final_head) ++heads;
        if (event.kind == ResponseEventKind::trailers) ++trailers;
        if (event.kind == ResponseEventKind::body) {
            CHECK(heads == 1 && event.body.size() <= body_window && !event.body.empty());
            body.append(reinterpret_cast<const char*>(event.body.data()), event.body.size());
            // Pausing after each fragment must leave all following payload intact.
            const auto paused = parser.consume_stream({}, 0);
            CHECK(paused.consumed == 0);
            // Empty input can report completion but cannot mutate body storage.
        }
        if (event.kind == ResponseEventKind::need_more && offset == wire.size()) CHECK(parser.finish_on_eof());
        if (event.kind == ResponseEventKind::complete) {
            CHECK(offset == wire.size() && heads == 1 && interim == expected_interim);
            CHECK(trailers == static_cast<unsigned>(expect_trailers));
            CHECK(parser.body_bytes() == body.size());
            return body;
        }
    }
    CHECK(false); return {};
}

void parser_streams() {
    const std::string chunked = "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3;x=\"a,b\"\r\nabc\r\n2\r\nde\r\n0\r\nX-Sum: ok\r\n\r\n";
    for (std::size_t split = 1; split <= chunked.size(); ++split) CHECK(stream_parse(chunked, split, 2, http::Method::GET, 1, true) == "abcde");
    const std::string large(128 * 1024, 'z');
    CHECK(stream_parse("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(large.size()) + "\r\n\r\n" + large, 4096, 17) == large);
    CHECK(stream_parse("HTTP/1.1 200 OK\r\n\r\n" + large, 2000, 13) == large);
    CHECK(stream_parse("HTTP/1.1 200 OK\r\nContent-Length: 9999999999\r\n\r\n", 1, 1, http::Method::HEAD).empty());
    CHECK(stream_parse("HTTP/1.1 204 No Content\r\n\r\n", 1, 1).empty());
    CHECK(stream_parse("HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\n", 5, 1).empty());

    http::ResponseParser parser;
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    auto event = parser.consume_stream(bytes(head + "abcde"), 0);
    CHECK(event.kind == ResponseEventKind::final_head && event.consumed == head.size());
    event = parser.consume_stream(bytes("abcde"), 0);
    CHECK(event.kind == ResponseEventKind::paused && event.consumed == 0);
    event = parser.consume_stream(bytes("abcdeNEXT"), 2);
    CHECK(event.kind == ResponseEventKind::body && event.consumed == 2);
    CHECK(!parser.finish_on_eof() && parser.error() == http::ParseError::premature_eof);
    for (const auto& malformed : {
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 3\r\n\r\n"}) {
        parser.reset(); std::size_t offset = 0;
        for (int i = 0; i < 10 && !parser.failed(); ++i) offset += parser.consume_stream(bytes(malformed).subspan(offset), 2).consumed;
        CHECK(parser.failed());
    }
    parser.reset();
    parser.consume_stream(bytes(head), 1);
    CHECK(parser.consume("abcde").event == http::ParseEvent::error); // cannot mix modes
}

std::string drain(http::ResponseWriter& writer) {
    std::string output;
    while (!writer.pending().empty()) {
        auto data = writer.pending(); const auto count = std::min<std::size_t>(3, data.size());
        output.append(reinterpret_cast<const char*>(data.data()), count);
        CHECK(writer.acknowledge(count));
    }
    return output;
}
void writer_streams() {
    http::ResponseWriter writer(256);
    CHECK(writer.begin({}, http::Method::GET, http::BodyFraming::chunked));
    CHECK(!writer.committed() && !writer.body(bytes("premature")));
    std::string wire = drain(writer);
    CHECK(writer.committed());
    for (int i = 0; i < 100; ++i) { CHECK(writer.body(bytes("abc"))); wire += drain(writer); }
    http::HeaderMap trailers; trailers.add("X-Total", "300");
    CHECK(writer.finish(trailers)); wire += drain(writer); CHECK(writer.complete());
    CHECK(stream_parse(wire, 19, 5, http::Method::GET, 0, true).size() == 300);
    CHECK(!writer.finish() && !writer.acknowledge(0));
    http::ResponseWriter truncated(256);
    CHECK(truncated.begin({}, http::Method::GET, http::BodyFraming::fixed_length, 4));
    (void)drain(truncated); CHECK(truncated.body(bytes("abc"))); (void)drain(truncated);
    CHECK(!truncated.finish() && !truncated.complete());
    truncated.abort(); CHECK(!truncated.complete() && truncated.pending().empty());
    http::ResponseWriter aborted(256);
    CHECK(aborted.begin({}, http::Method::GET, http::BodyFraming::chunked));
    auto prefix = drain(aborted); CHECK(aborted.body(bytes("abc"))); prefix += drain(aborted);
    aborted.abort(); CHECK(!aborted.finish() && prefix.find("0\r\n\r\n") == std::string::npos);
    for (const auto status : {http::StatusCode::OK, http::StatusCode::NoContent, http::StatusCode::NotModified}) {
        http::ResponseWriter no_body(256);
        http::Response head(status);
        const auto framing = status == http::StatusCode::NoContent ? http::BodyFraming::none : http::BodyFraming::fixed_length;
        const std::optional<std::uint64_t> length = status == http::StatusCode::NoContent ? std::nullopt : std::optional<std::uint64_t>(999);
        if (length) head.headers.set("Content-Length", "999");
        CHECK(no_body.begin(head, http::Method::HEAD, framing, length));
        (void)drain(no_body); CHECK(!no_body.body(bytes("x"))); CHECK(no_body.finish() && no_body.complete());
    }
    http::ResponseWriter bad(128);
    http::Response invalid; invalid.headers.add("X", "bad\r\ninjected");
    CHECK(!bad.begin(invalid, http::Method::GET, http::BodyFraming::fixed_length, 0));
}

void utilities_and_budget() {
    const auto canonical = test::take(http::parse_target("HTTP://Example.COM:80/a%2fb?b=2&a=1"));
    CHECK(canonical.key() == "http://example.com/a%2fb?b=2&a=1");
    CHECK(canonical == test::take(http::parse_target("/a%2fb?b=2&a=1", "example.com")));
    CHECK(canonical != test::take(http::parse_target("http://example.com/a/b?b=2&a=1")));
    CHECK(test::take(http::parse_target("http://[::1]:8080?x")).key() == "http://[::1]:8080/?x");
    for (const auto invalid : {"https://a/", "http://u:p@a/", "http://a:0/", "http://a:65536/", "http://a:80x/", "http://a/%xx", "http://a/#f", "http://[foo]/"}) CHECK(!http::parse_target(invalid));
    const auto now = std::chrono::system_clock::from_time_t(784111777);
    for (const auto date : {"Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994"}) CHECK(test::take(http::parse_http_date(date, now)) == now);
    const auto later = std::chrono::sys_days(std::chrono::year(2026)/1/1);
    CHECK(test::take(http::parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT", later)) == now);
    for (const auto date : {"Mon, 06 Nov 1994 08:49:37 GMT", "Sun, 31 Feb 1994 08:49:37 GMT", "Sun, 06 Nov 1994 24:49:37 GMT", "Sun, 06 Nov 1994 08:49:37 UTC"}) CHECK(!http::parse_http_date(date, now));
    const auto validators = test::take(http::parse_validator_list("W/\"a,b\", \"a,b\", \"c\\d\""));
    CHECK(validators.tags.size() == 3 && http::weak_match(validators.tags[0], validators.tags[1]));
    CHECK(test::take(http::parse_validator_list(" * \t")).wildcard);
    for (const auto invalid : {"", "*, \"x\"", "w/\"x\"", "\"x\",", "\"bad\n\"", "x"}) CHECK(!http::parse_validator_list(invalid));
    core::MemoryBudget budget(100);
    {
        auto reservation = test::take(budget.reserve(60));
        CHECK(!budget.reserve(41) && budget.used() == 60);
        CHECK(!reservation.resize(std::numeric_limits<std::size_t>::max()) && reservation.size() == 60);
        reservation.reclassify(core::AllocationKind::resident);
        CHECK(budget.used(core::AllocationKind::candidate) == 0 && budget.used(core::AllocationKind::resident) == 60);
        auto moved = std::move(reservation); CHECK(reservation.size() == 0);
        CHECK(moved.resize(100) && budget.used() == 100);
        moved.reclassify(core::AllocationKind::pinned); CHECK(budget.used(core::AllocationKind::pinned) == 100);
        CHECK(moved.resize(25) && budget.used() == 25);
        auto temporary = test::take(budget.reserve(75, core::AllocationKind::temporary));
        CHECK(!budget.reserve(1));
    }
    CHECK(budget.used() == 0);
    {
        std::vector<std::byte> data(50);
        CHECK(!cache::Body::create(data, test::take(budget.reserve(1))));
        CHECK(budget.used() == 0);
        auto body = test::take(cache::Body::create(std::move(data), test::take(budget.reserve(50))));
        auto reader = body;
        body->reclassify(core::AllocationKind::resident);
        body.reset();
        reader->reclassify(core::AllocationKind::pinned);
        CHECK(budget.used() == 50 && budget.used(core::AllocationKind::pinned) == 50);
        CHECK(reader->bytes().size() == 50);
    }
    CHECK(budget.used() == 0);
    core::RuntimeConfig config; CHECK(core::validate_config(config));
    config.buffers.low_water = config.buffers.high_water; CHECK(!core::validate_config(config));
}
} // namespace
int main() { parser_streams(); writer_streams(); utilities_and_budget(); }
