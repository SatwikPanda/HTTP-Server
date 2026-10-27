#include "http/router.hpp"

#include "fixtures/io_controls.hpp"
#include "http/http_session.hpp"
#include "test_support.hpp"

#include <stdexcept>
#include <string>

namespace {
std::string request(std::string target, std::string method = "GET", bool close = true) {
    return method + " " + target + " HTTP/1.1\r\nHost: localhost\r\nX-Name: Ada\r\n" +
        (close ? "Connection: close\r\n" : "") + "\r\n";
}

test::IndependentWireFrame roundtrip(const std::shared_ptr<const http::TransactionFactory>& factory,
    std::string wire, bool head = false, std::size_t limit = 4096) {
    http::HttpSessionConfig config;
    config.application = factory; config.keep_alive = true; config.max_output_bytes = limit;
    test::ControlledByteChannel channel(std::move(wire), 11, 7);
    const auto stats = http::run_http_session(channel, config);
    CHECK(stats.completed_cleanly && stats.peak_output_bytes <= limit);
    auto response = test::inspect_fixed_wire_response(channel.output(), head);
    CHECK(response.valid && response.total_wire_bytes == channel.output().size());
    return response;
}

void registration_and_matching() {
    http::Router router;
    const auto hello = [](const http::Request& req) {
        auto response = http::Response::text("Hello " + std::string(req.header("X-Name").value_or("world")));
        response.headers.add("X-Tag", "one"); response.headers.add("X-Tag", "two");
        return response;
    };
    for (const auto path : {"", "relative", "http://localhost/", "/bad?query", "/bad#fragment", "/bad%zz", "/bad\\path"}) {
        CHECK(!router.get(path, hello));
    }
    CHECK(!router.get("/empty", {}));
    CHECK(!router.get(std::string("/nul\0path", 9), hello));
    CHECK(!router.get("/" + std::string(8192, 'a'), hello));
    CHECK(router.get("/hello", hello));
    CHECK(!router.get("/hello", hello));
    CHECK(router.get("/item%2Fone", [](const http::Request&) { return http::Response::text("escaped"); }));
    auto first = router.factory();
    CHECK(router.get("/later", hello));
    CHECK(roundtrip(first, request("/later")).status_code == 404);
    CHECK(roundtrip(router.factory(), request("/later")).status_code == 200);
    for (const auto target : {"/hello", "/hello?name=ignored", "http://localhost/hello?x=1"}) {
        const auto response = roundtrip(first, request(target));
        CHECK(response.status_code == 200 && response.body == "Hello Ada");
        CHECK(response.has_header_line("Content-Type: text/plain; charset=utf-8"));
        CHECK(response.has_header_line("X-Tag: one") && response.has_header_line("X-Tag: two"));
    }
    for (const auto target : {"/Hello", "/hello/", "/hello/world", "/item/one"}) {
        CHECK(roundtrip(first, request(target)).status_code == 404);
    }
    CHECK(roundtrip(first, request("/item%2Fone")).body == "escaped");
    CHECK(roundtrip(first, request("/bad%zz")).status_code == 400);
    const auto head = roundtrip(first, request("/hello", "HEAD"), true);
    CHECK(head.status_code == 200 && head.body.empty() && head.has_header_line("Content-Length: 9"));
    const auto post = roundtrip(first, request("/hello", "POST"));
    CHECK(post.status_code == 405 && post.has_header_line("Allow: GET, HEAD"));

    http::Router bounded;
    for (std::size_t i = 0; i < http::Router::max_routes; ++i) CHECK(bounded.get("/" + std::to_string(i), hello));
    CHECK(!bounded.get("/overflow", hello));
}

void failures_and_limits() {
    http::Router router;
    CHECK(router.get("/throws", [](const http::Request&) -> http::Response { throw std::runtime_error("secret"); }));
    CHECK(router.get("/error", [](const http::Request&) -> core::Result<http::Response> {
        return core::make_error(core::ErrorCategory::io_error, "private application detail");
    }));
    CHECK(router.get("/large", [](const http::Request&) { return http::Response::text(std::string(1024, 'a')); }));
    CHECK(router.get("/header", [](const http::Request&) {
        auto response = http::Response::text("ok"); response.headers.add("X-Bad", "value\r\nInjected: yes"); return response;
    }));
    CHECK(router.get("/upgrade", [](const http::Request&) { return http::Response(http::StatusCode::SwitchingProtocols); }));
    CHECK(router.get("/created", [](const http::Request&) { return http::Response::text("created", http::StatusCode::Created); }));
    const auto factory = router.factory();
    for (const auto target : {"/throws", "/error", "/large", "/header", "/upgrade"}) {
        const auto response = roundtrip(factory, request(target), false, 512);
        CHECK(response.status_code == 500 && response.has_header_line("Connection: close"));
        CHECK(response.body.find("secret") == std::string::npos);
        CHECK(response.raw_header_block.find("Injected") == std::string::npos);
    }
    const auto created = roundtrip(factory, request("/created"));
    CHECK(created.status_code == 201 && created.body == "created");
}

void pipeline_and_lifetime() {
    std::size_t calls = 0;
    std::shared_ptr<const http::TransactionFactory> factory;
    {
        http::Router router;
        CHECK(router.get("/", [&](const http::Request& req) {
            ++calls;
            return http::Response::text(std::string("a\0b", 3) + req.target);
        }));
        factory = router.factory();
    }
    http::HttpSessionConfig config;
    config.application = factory; config.keep_alive = true; config.max_output_bytes = 512;
    test::ControlledByteChannel channel(request("/", "GET", false) + request("/", "HEAD", false) +
        request("/missing", "GET", false) + request("/?last=1"), 23, 5);
    channel.set_write_would_block_every(5);
    const auto stats = http::run_http_session(channel, config);
    CHECK(stats.completed_cleanly && stats.requests_served == 4 && calls == 3);
    CHECK(stats.short_write_count > 0 && stats.would_block_write_count > 0);
    std::string_view remaining = channel.output();
    const auto first = test::inspect_fixed_wire_response(remaining); remaining.remove_prefix(first.total_wire_bytes);
    const auto head = test::inspect_fixed_wire_response(remaining, true); remaining.remove_prefix(head.total_wire_bytes);
    const auto missing = test::inspect_fixed_wire_response(remaining); remaining.remove_prefix(missing.total_wire_bytes);
    const auto last = test::inspect_fixed_wire_response(remaining); remaining.remove_prefix(last.total_wire_bytes);
    CHECK(first.valid && head.valid && missing.valid && last.valid && remaining.empty());
    CHECK(first.body == std::string("a\0b/", 4) && head.body.empty() && missing.status_code == 404);
    CHECK(last.body == std::string("a\0b/?last=1", 11));

    http::Request req; req.headers.add("Host", "localhost");
    auto transaction = test::take(factory->create(req, {{{1, 1}, 1}, 512, core::real_steady_clock(), core::real_system_clock()}));
    factory.reset(); config.application.reset();
    CHECK(std::holds_alternative<http::BufferedResponse>(transaction->on_event(http::StartTransaction{})));
    CHECK(std::holds_alternative<http::TransactionFailure>(transaction->on_event(http::StartTransaction{})));
}
} // namespace

int main() {
    registration_and_matching(); failures_and_limits(); pipeline_and_lifetime();
}
