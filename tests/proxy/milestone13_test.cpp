// Milestone 13: proxy targets and outbound request preparation. No origin needed.
#include "test_support.hpp"
#include "http/serializer.hpp"
#include "proxy/forwarding.hpp"

#include <string>
#include <vector>

namespace {
http::Request client(std::string target, std::vector<http::HeaderField> fields = {},
                     http::HttpVersion version = http::HttpVersion::http_1_1()) {
    http::Request request;
    request.target = std::move(target);
    request.version = version;
    for (auto& field : fields) request.headers.add(field.name, field.value);
    return request;
}

std::string wire(const proxy::OutboundRequest& outbound) {
    return test::take(http::serialize_request_head(outbound.request));
}

void targets_and_ports() {
    struct Case { const char* target; const char* host; std::uint16_t port; const char* authority; const char* path; };
    const Case cases[] = {
        {"http://Example.COM/a", "example.com", 80, "example.com", "/a"},
        {"HTTP://example.com:80/a", "example.com", 80, "example.com", "/a"},
        {"http://example.com:8080/a?x=1", "example.com", 8080, "example.com:8080", "/a?x=1"},
        {"http://example.com", "example.com", 80, "example.com", "/"},
        {"http://example.com?q=1", "example.com", 80, "example.com", "/?q=1"},
        {"http://[::1]:8080/v6", "::1", 8080, "[::1]:8080", "/v6"},
        {"http://[::1]/v6", "::1", 80, "[::1]", "/v6"},
        {"http://127.0.0.1:9/%7Euser/a%2Fb?z=2&a=1&z=1", "127.0.0.1", 9, "127.0.0.1:9", "/%7Euser/a%2Fb?z=2&a=1&z=1"},
    };
    for (const auto& c : cases) {
        auto prepared = proxy::prepare_outbound(client(c.target), {});
        CHECK(prepared);
        const auto& outbound = *prepared.outbound;
        CHECK(outbound.resolve.host == c.host && outbound.resolve.port == c.port);
        CHECK(outbound.request.target == c.path);
        CHECK(outbound.request.headers.get("Host") == c.authority);
        CHECK(wire(outbound).starts_with(std::string("GET ") + c.path + " HTTP/1.1\r\nHost: " + c.authority + "\r\n"));
    }
    const char* rejected[] = {
        "/origin-form", "*", "https://example.com/", "ftp://example.com/", "http://user@example.com/",
        "http://user:pw@example.com/", "http://example.com/#frag", "http://[::1/", "http://[zz]/",
        "http://example.com:0/", "http://example.com:65536/", "http://example.com:x/", "http:///path",
        "http://exa mple.com/", "http://example.com/%zz",
    };
    for (const auto* target : rejected) {
        auto prepared = proxy::prepare_outbound(client(target), {});
        CHECK(!prepared && prepared.status == http::StatusCode::BadRequest && prepared.error);
    }
}

void header_rewriting() {
    auto prepared = proxy::prepare_outbound(client("http://origin.test:81/p", {
        {"Host", "attacker.test"}, {"Host", "second.test"},
        {"Connection", "X-Hop, keep-alive"}, {"connection", "x-other"},
        {"X-Hop", "drop"}, {"X-Other", "drop"}, {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"}, {"Proxy-Authorization", "Basic c2VjcmV0"},
        {"TE", "trailers"}, {"Trailer", "X"}, {"Content-Length", "0"},
        {"Accept", "a"}, {"Accept", "b"}, {"If-None-Match", "\"v1\""}, {"Range", "bytes=0-1"},
        {"Authorization", "Bearer origin-token"}, {"Via", "1.1 upstream-a"},
    }), {});
    CHECK(prepared);
    const auto& headers = prepared.outbound->request.headers;
    CHECK(headers.get_all("Host").size() == 1 && headers.get("Host") == "origin.test:81");
    for (const auto* name : {"X-Hop", "X-Other", "Keep-Alive", "Proxy-Connection", "Proxy-Authorization",
                             "TE", "Trailer", "Content-Length", "Transfer-Encoding", "Upgrade"}) {
        CHECK(!headers.contains(name));
    }
    CHECK(headers.get_all("Connection").size() == 1 && headers.get("Connection") == "close");
    const auto accepts = headers.get_all("Accept");
    CHECK(accepts.size() == 2 && accepts[0] == "a" && accepts[1] == "b");
    CHECK(headers.get("If-None-Match") == "\"v1\"" && headers.get("Range") == "bytes=0-1");
    CHECK(headers.get("Authorization") == "Bearer origin-token"); // end-to-end credentials
    const auto via = headers.get_all("Via");
    CHECK(via.size() == 2 && via[0] == "1.1 upstream-a" && via[1] == "1.1 httpserver");
    const auto text = wire(*prepared.outbound);
    CHECK(text.find("secret") == std::string::npos && text.find("c2VjcmV0") == std::string::npos);
    CHECK(text.ends_with("Connection: close\r\n\r\n"));

    auto head = proxy::prepare_outbound(client("http://o/", {}, http::HttpVersion::http_1_0()), {});
    CHECK(head && head.outbound->request.version == http::HttpVersion::http_1_1());
    CHECK(head.outbound->request.headers.get("Via") == "1.0 httpserver");
    auto method = client("http://o/"); method.method = http::Method::HEAD;
    auto prepared_head = proxy::prepare_outbound(method, {});
    CHECK(prepared_head && wire(*prepared_head.outbound).starts_with("HEAD / HTTP/1.1\r\n"));

    auto bad_connection = proxy::prepare_outbound(client("http://o/", {{"Connection", "bad token"}}), {});
    CHECK(!bad_connection && bad_connection.status == http::StatusCode::BadRequest);
}

void loops_and_limits() {
    for (const char* via : {"1.1 httpserver", "1.0 a, 1.1 HTTPSERVER (comment)", "HTTP/1.1 httpserver"}) {
        auto loop = proxy::prepare_outbound(client("http://o/", {{"Via", via}}), {});
        CHECK(!loop && loop.status == http::StatusCode::Forbidden);
    }
    auto other = proxy::prepare_outbound(client("http://o/", {{"Via", "1.1 httpserver-two"}}), {});
    CHECK(other);
    proxy::ForwardingConfig small; small.max_via_bytes = 8;
    auto long_via = proxy::prepare_outbound(client("http://o/", {{"Via", "1.1 long-chain"}}), small);
    CHECK(!long_via && long_via.status == http::StatusCode::BadRequest);
    proxy::ForwardingConfig invalid; invalid.via_pseudonym = "bad name";
    CHECK(!proxy::validate_forwarding_config(invalid));
    CHECK(!proxy::prepare_outbound(client("http://o/"), invalid));
    proxy::ForwardingConfig custom; custom.via_pseudonym = "edge-1";
    auto renamed = proxy::prepare_outbound(client("http://o/", {{"Via", "1.1 httpserver"}}), custom);
    CHECK(renamed && renamed.outbound->request.headers.get_all("Via").back() == "1.1 edge-1");
}

void response_direction() {
    http::Response origin(200, "Fine");
    origin.version = http::HttpVersion::http_1_0();
    for (const auto& [name, value] : std::vector<std::pair<const char*, const char*>>{
             {"Connection", "X-Secret"}, {"X-Secret", "1"}, {"Keep-Alive", "a"},
             {"Transfer-Encoding", "chunked"}, {"Content-Length", "5"}, {"Proxy-Authenticate", "Basic"},
             {"Set-Cookie", "a=1, b"}, {"Set-Cookie", "c=2"}, {"Content-Encoding", "gzip"}, {"Via", "1.1 o"}}) {
        origin.headers.add(name, value);
    }
    auto head = test::take(proxy::prepare_downstream_head(origin, {}));
    CHECK(head.status_code == 200 && head.reason == "Fine");
    for (const auto* name : {"Connection", "X-Secret", "Keep-Alive", "Transfer-Encoding", "Content-Length", "Proxy-Authenticate"}) {
        CHECK(!head.headers.contains(name));
    }
    const auto cookies = head.headers.get_all("Set-Cookie");
    CHECK(cookies.size() == 2 && cookies[0] == "a=1, b" && cookies[1] == "c=2");
    CHECK(head.headers.get("Content-Encoding") == "gzip");
    const auto via = head.headers.get_all("Via");
    CHECK(via.size() == 2 && via[1] == "1.0 httpserver");

    http::Response looped(200, "OK"); looped.headers.add("Via", "1.1 httpserver");
    CHECK(!proxy::prepare_downstream_head(looped, {}));
    CHECK(!proxy::prepare_downstream_head(http::Response(101, "Switching Protocols"), {}));

    http::HeaderMap trailers; trailers.add("X-Checksum", "1"); trailers.add("Content-Length", "9");
    trailers.add("Transfer-Encoding", "x"); trailers.add("X-Nominated", "1"); trailers.add("Set-Cookie", "z");
    http::HeaderMap trailer_head; trailer_head.add("Connection", "X-Nominated");
    const auto kept = proxy::forwardable_trailers(trailers, trailer_head);
    CHECK(kept.size() == 1 && kept.get("X-Checksum") == "1");
    CHECK(proxy::is_hop_by_hop("proxy-authorization") && !proxy::is_hop_by_hop("Authorization"));
}
} // namespace

int main() {
    targets_and_ports();
    header_rewriting();
    loops_and_limits();
    response_direction();
    std::cout << "milestone 13 proxy preparation passed\n";
    return 0;
}
