// Milestone 15: chunked, close-delimited, interim, bodyless and trailer forms
// with independently selected downstream framing.
#include "proxy_test_support.hpp"
#include "http/serializer.hpp"

namespace {
using namespace proxy_test;

struct Run { http::HttpSessionStats stats; std::string output; std::vector<std::string> requests; proxy::ProxyStats::Snapshot snapshot; };

Run run(std::vector<OriginResponse> responses, const std::function<std::string(const MultiOrigin&)>& input,
        std::size_t origin_window = 97, std::size_t window = 256) {
    MultiOrigin origin(std::move(responses), origin_window);
    auto stats = std::make_shared<proxy::ProxyStats>();
    Channel channel(input(origin), 13);
    Run result;
    result.stats = http::run_http_session(channel, session_config(stats, {}, window));
    origin.join();
    result.output = channel.output;
    result.requests = origin.requests();
    result.snapshot = stats->snapshot();
    if (!result.stats.completed_cleanly) std::cerr << "session error: " << result.stats.error.to_string() << "\n" << result.output.substr(0, 400) << '\n';
    CHECK(result.snapshot.active == 0);
    return result;
}

void chunked_to_chunked_with_trailers() {
    const std::string body(5000, 'c');
    const auto chunk = test::take(http::encode_chunk(body.substr(0, 3000))) + test::take(http::encode_chunk(body.substr(3000)));
    auto result = run({{{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Encoding: gzip\r\nTrailer: X-Sum\r\n\r\n",
        chunk, "0\r\nX-Sum: 42\r\nVia: 1.1 smuggled\r\n\r\n"}}}, [](const MultiOrigin& o) { return get(o.url("/c")); }, 17);
    CHECK(result.stats.completed_cleanly && result.stats.peak_output_bytes <= 256);
    auto parsed = parse_all(result.output, {http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].response.body == body);
    CHECK(parsed[0].response.headers.get("Transfer-Encoding") == "chunked");
    CHECK(parsed[0].response.headers.get("Content-Encoding") == "gzip"); // opaque, not decoded
    CHECK(!parsed[0].response.headers.contains("Trailer"));
    CHECK(parsed[0].response.trailers.get("X-Sum") == "42" && !parsed[0].response.trailers.contains("Via"));
    CHECK(result.snapshot.completed == 1);
}

void close_delimited_to_chunked_and_http10() {
    const std::string body(3000, 'd');
    auto result = run({{{"HTTP/1.1 200 OK\r\nContent-Type: x/y\r\n\r\n", body}}},
        [](const MultiOrigin& o) { return get(o.url("/d")); }, 41);
    CHECK(result.stats.completed_cleanly);
    auto parsed = parse_all(result.output, {http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].response.body == body);
    CHECK(parsed[0].response.headers.get("Transfer-Encoding") == "chunked");

    // HTTP/1.0 client: unknown length becomes close-delimited HTTP/1.0 output.
    auto old = run({{{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", test::take(http::encode_chunk(body)), "0\r\n\r\n"}}},
        [](const MultiOrigin& o) { return get(o.url("/old"), "GET", false, "HTTP/1.0"); });
    CHECK(old.stats.completed_cleanly);
    CHECK(old.output.starts_with("HTTP/1.0 200") && old.output.find("Transfer-Encoding") == std::string::npos);
    CHECK(old.output.find("Content-Length") == std::string::npos && old.output.ends_with("\r\n\r\n" + body));
    CHECK(old.requests[0].find("Via: 1.0 httpserver\r\n") != std::string::npos);
}

void interim_and_bodyless_over_persistent_connection() {
    // Five requests share one client connection; each uses its own origin connection.
    auto result = run({
        {{"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </s>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"}},
        {{"HTTP/1.1 204 No Content\r\nX-A: 1\r\n\r\n"}},
        {{"HTTP/1.1 304 Not Modified\r\nETag: \"e\"\r\nContent-Length: 77\r\n\r\n"}},
        {{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"}},
        {{"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nlast"}},
    }, [](const MultiOrigin& o) {
        return get(o.url("/1"), "GET", false) + get(o.url("/2"), "GET", false) +
               get(o.url("/3"), "GET", false, "HTTP/1.1", "If-None-Match: \"e\"\r\n") +
               get(o.url("/4"), "HEAD", false) + get(o.url("/5"));
    }, 5);
    CHECK(result.stats.completed_cleanly && result.stats.requests_served == 5);
    CHECK(result.requests.size() == 5 && result.requests[2].find("If-None-Match: \"e\"\r\n") != std::string::npos);
    CHECK(result.requests[3].starts_with("HEAD /4 "));
    auto parsed = parse_all(result.output, {http::Method::GET, http::Method::GET, http::Method::GET, http::Method::HEAD, http::Method::GET});
    std::size_t consumed = 0;
    for (const auto& p : parsed) { CHECK(p.complete); consumed += p.consumed; }
    CHECK(consumed == result.output.size());
    CHECK(parsed[0].response.status_code == 200 && parsed[0].response.body == "abc");
    CHECK(result.output.find("103") == std::string::npos && result.output.find("100 Continue") == std::string::npos);
    CHECK(parsed[1].response.status_code == 204 && parsed[1].response.headers.get("X-A") == "1");
    CHECK(!parsed[1].response.headers.contains("Content-Length") && !parsed[1].response.headers.contains("Transfer-Encoding"));
    CHECK(parsed[2].response.status_code == 304 && parsed[2].response.headers.get("Content-Length") == "77" && parsed[2].response.body.empty());
    CHECK(parsed[3].response.status_code == 200 && parsed[3].response.body.empty());
    CHECK(!parsed[3].response.headers.contains("Transfer-Encoding"));
    CHECK(parsed[4].response.body == "last");
    CHECK(result.snapshot.completed == 5 && result.snapshot.origin_connections == 5);
}

void unsupported_switch_and_bodyless_extra_bytes() {
    auto switched = run({{{"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: upgrade\r\n\r\n"}}},
        [](const MultiOrigin& o) { return get(o.url("/ws")); });
    CHECK(switched.output.starts_with("HTTP/1.1 502"));
    CHECK(switched.snapshot.bad_gateway == 1);

    // Unexpected bytes after a HEAD/204 response never become another response.
    auto extra = run({{{"HTTP/1.1 204 No Content\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nsmug"}},
                      {{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}}},
        [](const MultiOrigin& o) { return get(o.url("/a"), "GET", false) + get(o.url("/b")); });
    auto parsed = parse_all(extra.output, {http::Method::GET, http::Method::GET});
    CHECK(parsed[0].response.status_code == 204 && parsed[1].response.body == "ok");
    CHECK(extra.output.find("smug") == std::string::npos);
}

void split_at_every_boundary() {
    // The same chunked response with trailers, delivered at write windows 1..N.
    const std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nwiki\r\n5\r\npedia\r\n0\r\nX-T: v\r\n\r\n";
    for (std::size_t window = 1; window <= 24; ++window) {
        auto result = run({{{wire}}}, [](const MultiOrigin& o) { return get(o.url("/split")); }, window);
        auto parsed = parse_all(result.output, {http::Method::GET});
        CHECK(parsed[0].complete && parsed[0].response.body == "wikipedia" && parsed[0].response.trailers.get("X-T") == "v");
    }
    // Segment boundaries placed inside the status line, a header and the body.
    const std::string fixed = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nX-Y: z\r\n\r\n0123456789";
    for (std::size_t cut = 1; cut < fixed.size(); cut += 3) {
        auto result = run({{{fixed.substr(0, cut), fixed.substr(cut)}}}, [](const MultiOrigin& o) { return get(o.url("/cut")); });
        auto parsed = parse_all(result.output, {http::Method::GET});
        CHECK(parsed[0].complete && parsed[0].response.body == "0123456789" && parsed[0].response.headers.get("X-Y") == "z");
    }
}
} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    chunked_to_chunked_with_trailers();
    close_delimited_to_chunked_and_http10();
    interim_and_bodyless_over_persistent_connection();
    unsupported_switch_and_bodyless_extra_bytes();
    split_at_every_boundary();
    std::cout << "milestone 15 proxy streaming passed\n";
    return 0;
}
