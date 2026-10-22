// Milestone 14: forward complete fixed-length origin responses through the
// production HttpSession, proxy transaction and reference work executor.
#include "proxy_test_support.hpp"

namespace {
using namespace proxy_test;

void fixed_get_with_fragmentation() {
    const std::string body = "hello through the proxy";
    test::ControlledOrigin origin({"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: X-Private\r\nX-Private: secret\r\nKeep-Alive: timeout=5\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Content-Type: text/plain\r\nDate: Thu, 01 Jan 2026 00:00:00 GMT\r\n\r\n", body}, 3);
    auto stats = std::make_shared<proxy::ProxyStats>();
    const auto url = "http://" + origin.target().host + ":" + std::to_string(origin.target().port) + "/a%2Fb?x=1&a=2";
    Channel channel(get(url, "GET", true, "HTTP/1.1", "Proxy-Authorization: Basic Zm9v\r\nVia: 1.1 other\r\n"), 5, 3);
    const auto result = http::run_http_session(channel, session_config(stats));
    CHECK(result.completed_cleanly && result.requests_served == 1 && result.short_write_count > 0);
    origin.join();
    const auto& request = origin.request();
    const auto authority = origin.target().host + ":" + std::to_string(origin.target().port);
    CHECK(request.starts_with("GET /a%2Fb?x=1&a=2 HTTP/1.1\r\nHost: " + authority + "\r\n"));
    CHECK(request.find("client-supplied") == std::string::npos && request.find("Zm9v") == std::string::npos);
    CHECK(request.find("Via: 1.1 other\r\nVia: 1.1 httpserver\r\n") != std::string::npos);
    CHECK(request.ends_with("Connection: close\r\n\r\n"));

    auto parsed = parse_all(channel.output, {http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].consumed == channel.output.size());
    const auto& response = parsed[0].response;
    CHECK(response.status_code == 200 && response.body == body);
    CHECK(response.headers.get("Content-Length") == std::to_string(body.size()));
    CHECK(!response.headers.contains("X-Private") && !response.headers.contains("Keep-Alive"));
    CHECK(response.headers.get_all("Set-Cookie").size() == 2);
    CHECK(response.headers.get("Content-Type") == "text/plain");
    CHECK(response.headers.get("Date") == "Thu, 01 Jan 2026 00:00:00 GMT");
    CHECK(response.headers.get("Via") == "1.1 httpserver");
    CHECK(response.headers.get("Connection") == "close");
    const auto snapshot = stats->snapshot();
    CHECK(snapshot.completed == 1 && snapshot.active == 0 && snapshot.origin_connections == 1);
}

void head_metadata() {
    test::ControlledOrigin origin({"HTTP/1.1 200 OK\r\nContent-Length: 123456\r\nETag: \"v\"\r\n\r\n"});
    auto stats = std::make_shared<proxy::ProxyStats>();
    const auto url = "http://" + origin.target().host + ":" + std::to_string(origin.target().port) + "/head";
    Channel channel(get(url, "HEAD"));
    const auto result = http::run_http_session(channel, session_config(stats));
    CHECK(result.completed_cleanly && result.requests_served == 1);
    origin.join();
    CHECK(origin.request().starts_with("HEAD /head HTTP/1.1\r\n"));
    auto parsed = parse_all(channel.output, {http::Method::HEAD});
    CHECK(parsed[0].complete && parsed[0].consumed == channel.output.size());
    CHECK(parsed[0].response.headers.get("Content-Length") == "123456" && parsed[0].response.body.empty());
    CHECK(parsed[0].response.headers.get("ETag") == "\"v\"");
}

void keep_alive_uses_fresh_origin_connections() {
    MultiOrigin origin({{{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst"}},
                        {{"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond"}}}, 4);
    auto stats = std::make_shared<proxy::ProxyStats>();
    Channel channel(get(origin.url("/1"), "GET", false) + get(origin.url("/2"), "GET", true), 11);
    const auto result = http::run_http_session(channel, session_config(stats));
    CHECK(result.completed_cleanly && result.requests_served == 2);
    origin.join();
    CHECK(origin.connections() == 2 && origin.requests().size() == 2);
    CHECK(origin.requests()[0].starts_with("GET /1 ") && origin.requests()[1].starts_with("GET /2 "));
    auto parsed = parse_all(channel.output, {http::Method::GET, http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].response.body == "first");
    CHECK(parsed[0].response.headers.get("Connection") == "keep-alive");
    CHECK(parsed[1].complete && parsed[1].response.body == "second");
    CHECK(parsed[0].consumed + parsed[1].consumed == channel.output.size());
    const auto snapshot = stats->snapshot();
    CHECK(snapshot.completed == 2 && snapshot.origin_connections == 2 && snapshot.active == 0 && snapshot.peak_active == 1);
}

void real_loopback_downstream() {
    const std::string body(20000, 'r');
    MultiOrigin origin({{{"HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n", body}}}, 31);
    auto stats = std::make_shared<proxy::ProxyStats>();
    auto pair = test::pair();
    http::HttpSessionStats result;
    auto config = session_config(stats);
    std::thread server([&] { result = http::run_http_session(pair.server, config); pair.server.close(); });
    auto poller = test::take(net::Poller::create());
    auto token = test::take(poller.watch(pair.client, net::Interest::write, {3, 1}));
    const auto deadline = test::Clock::now() + 10s;
    auto wait = [&](net::Interest interest) {
        CHECK(poller.modify(token, interest));
        while (test::Clock::now() < deadline) {
            auto events = test::take(poller.wait(deadline));
            for (const auto& event : events) if (event.token == token && poller.is_current(event)) return;
        }
        CHECK(false);
    };
    const auto outbound = get(origin.url("/real"));
    std::size_t offset = 0;
    while (offset < outbound.size()) {
        auto wrote = pair.client.write_some(bytes(outbound).subspan(offset));
        if (wrote.is_ok()) offset += wrote.bytes_transferred;
        else { CHECK(wrote.would_block()); wait(net::Interest::write); }
    }
    std::array<std::byte, 89> input{}; std::string output;
    for (;;) {
        auto read = pair.client.read_some(input);
        if (read.is_eof()) break;
        if (read.is_ok()) output.append(reinterpret_cast<const char*>(input.data()), read.bytes_transferred);
        else { CHECK(read.would_block()); wait(net::Interest::read); }
    }
    CHECK(poller.unwatch(token)); server.join(); origin.join();
    CHECK(result.completed_cleanly && result.requests_served == 1 && result.peak_output_bytes <= 256);
    auto parsed = parse_all(output, {http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].response.body == body);
}

void ipv6_origin_when_available() {
    auto probe = net::create_tcp_socket(net::AddressFamily::ipv6);
    if (!probe || !probe.value().bind(net::Endpoint::ipv6_loopback(0))) {
        std::cout << "IPv6 loopback unavailable; IPv6 proxy coverage skipped\n";
        return;
    }
    probe.value().close();
    MultiOrigin origin({{{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nv6"}}}, 97, net::AddressFamily::ipv6);
    auto stats = std::make_shared<proxy::ProxyStats>();
    Channel channel(get(origin.url("/six")));
    CHECK(http::run_http_session(channel, session_config(stats)).completed_cleanly);
    origin.join();
    CHECK(origin.requests()[0].find("Host: " + origin.authority() + "\r\n") != std::string::npos);
    auto parsed = parse_all(channel.output, {http::Method::GET});
    CHECK(parsed[0].complete && parsed[0].response.body == "v6");
}
} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    fixed_get_with_fragmentation();
    head_metadata();
    keep_alive_uses_fresh_origin_connections();
    real_loopback_downstream();
    ipv6_origin_when_available();
    std::cout << "milestone 14 proxy forwarding passed\n";
    return 0;
}
