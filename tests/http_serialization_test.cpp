#include "http/date.hpp"
#include "http/header.hpp"
#include "http/method.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/serializer.hpp"
#include "http/status.hpp"
#include "http/version.hpp"

#include "test_support.hpp"
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

void test_http_methods() {
    std::cout << "[RUN] test_http_methods..." << std::endl;
    CHECK(http::to_string(http::Method::GET) == "GET");
    CHECK(http::to_string(http::Method::HEAD) == "HEAD");
    CHECK(http::to_string(http::Method::POST) == "POST");

    CHECK(http::method_from_string("GET") == http::Method::GET);
    CHECK(http::method_from_string("HEAD") == http::Method::HEAD);
    CHECK(!http::method_from_string("INVALID").has_value());

    CHECK(http::is_safe_method(http::Method::GET));
    CHECK(http::is_safe_method(http::Method::HEAD));
    CHECK(!http::is_safe_method(http::Method::POST));

    CHECK(http::is_idempotent_method(http::Method::GET));
    CHECK(http::is_idempotent_method(http::Method::PUT));
    CHECK(!http::is_idempotent_method(http::Method::POST));
    std::cout << "[PASS] test_http_methods" << std::endl;
}

void test_http_status_and_reason() {
    std::cout << "[RUN] test_http_status_and_reason..." << std::endl;
    CHECK(http::default_reason_phrase(http::StatusCode::OK) == "OK");
    CHECK(http::default_reason_phrase(http::StatusCode::NotFound) == "Not Found");
    CHECK(http::default_reason_phrase(http::StatusCode::BadRequest) == "Bad Request");
    CHECK(http::default_reason_phrase(http::StatusCode::HeaderFieldsTooLarge) == "Request Header Fields Too Large");
    CHECK(http::default_reason_phrase(http::StatusCode::InternalServerError) == "Internal Server Error");

    CHECK(http::is_valid_status_code(100));
    CHECK(http::is_valid_status_code(200));
    CHECK(http::is_valid_status_code(505));
    CHECK(http::is_valid_status_code(599));
    CHECK(!http::is_valid_status_code(600));
    CHECK(!http::is_valid_status_code(999));
    CHECK(!http::is_valid_status_code(99));
    CHECK(!http::is_valid_status_code(1000));

    CHECK(http::is_valid_reason_phrase("OK"));
    CHECK(http::is_valid_reason_phrase("Not Found"));
    CHECK(!http::is_valid_reason_phrase("Bad\r\nPhrase"));
    CHECK(!http::is_valid_reason_phrase("Bad\nPhrase"));
    std::cout << "[PASS] test_http_status_and_reason" << std::endl;
}

void test_http_version() {
    std::cout << "[RUN] test_http_version..." << std::endl;
    http::HttpVersion v11 = http::HttpVersion::http_1_1();
    CHECK(v11.major == 1 && v11.minor == 1);
    CHECK(v11.to_string() == "HTTP/1.1");
    CHECK(v11.is_valid());

    http::HttpVersion v10 = http::HttpVersion::http_1_0();
    CHECK(v10 < v11);
    CHECK(v10 != v11);
    CHECK((v11 == http::HttpVersion{1, 1}));
    std::cout << "[PASS] test_http_version" << std::endl;
}

void test_header_validation() {
    std::cout << "[RUN] test_header_validation..." << std::endl;
    // Valid header names
    CHECK(http::is_valid_header_name("Content-Type"));
    CHECK(http::is_valid_header_name("Content-Length"));
    CHECK(http::is_valid_header_name("Host"));
    CHECK(http::is_valid_header_name("X-Custom_Header.1"));
    CHECK(http::is_valid_header_name("ETag"));

    // Invalid header names
    CHECK(!http::is_valid_header_name(""));
    CHECK(!http::is_valid_header_name("Content Type")); // Space
    CHECK(!http::is_valid_header_name("Header:1"));     // Colon
    CHECK(!http::is_valid_header_name("Header\r\n"));   // CRLF
    CHECK(!http::is_valid_header_name("Header@"));      // @ not tchar
    CHECK(!http::is_valid_header_name("Header[x]"));    // Brackets

    // Valid header values
    CHECK(http::is_valid_header_value("text/plain; charset=utf-8"));
    CHECK(http::is_valid_header_value("12345"));
    CHECK(http::is_valid_header_value(""));
    CHECK(http::is_valid_header_value("gzip, deflate"));
    CHECK(http::is_valid_header_value("foo\tbar"));

    // Invalid header values (CRLF injection prevention)
    CHECK(!http::is_valid_header_value("value\r\nInjected: evil"));
    CHECK(!http::is_valid_header_value("value\ninjection"));
    CHECK(!http::is_valid_header_value("value\r"));
    std::string null_injected = "val";
    null_injected.push_back('\0');
    null_injected += "ue";
    CHECK(!http::is_valid_header_value(null_injected));
    std::cout << "[PASS] test_header_validation" << std::endl;
}

void test_header_map_repeated_and_case_insensitive() {
    std::cout << "[RUN] test_header_map_repeated_and_case_insensitive..." << std::endl;
    http::HeaderMap map;

    // Test case-insensitive lookup
    map.set("Content-Type", "text/html");
    CHECK(map.contains("content-type"));
    CHECK(map.contains("CONTENT-TYPE"));
    CHECK(map.contains("Content-Type"));
    CHECK(!map.contains("Content-Length"));

    auto ct = map.get("content-type");
    CHECK(ct.has_value());
    CHECK(ct.value() == "text/html");

    // Test repeated fields without joining
    map.add("Set-Cookie", "session_id=123; Path=/");
    map.add("Set-Cookie", "theme=dark; Path=/");
    map.add("set-cookie", "lang=en; Path=/");

    CHECK(map.size() == 4);
    auto cookies = map.get_all("SET-COOKIE");
    CHECK(cookies.size() == 3);
    CHECK(cookies[0] == "session_id=123; Path=/");
    CHECK(cookies[1] == "theme=dark; Path=/");
    CHECK(cookies[2] == "lang=en; Path=/");

    // Test set replaces case-insensitively
    map.set("content-type", "application/json");
    CHECK(map.get("Content-Type").value() == "application/json");
    CHECK(map.get_all("Content-Type").size() == 1);

    // Test remove
    CHECK(map.remove("set-cookie"));
    CHECK(!map.contains("Set-Cookie"));
    CHECK(map.get_all("Set-Cookie").empty());
    CHECK(map.size() == 1);

    std::cout << "[PASS] test_header_map_repeated_and_case_insensitive" << std::endl;
}

void test_http_date_formatting() {
    std::cout << "[RUN] test_http_date_formatting..." << std::endl;
    // Fixed timestamp: 784111777 is Sun, 06 Nov 1994 08:49:37 GMT
    auto tp = std::chrono::system_clock::from_time_t(784111777);
    std::string date_str = http::format_http_date(tp);
    CHECK(date_str == "Sun, 06 Nov 1994 08:49:37 GMT");

    // Current time formatting should produce 29 characters ending in " GMT"
    std::string now_str = http::format_http_date();
    CHECK(now_str.size() == 29);
    CHECK(now_str.substr(25) == " GMT");
    std::cout << "[PASS] test_http_date_formatting" << std::endl;
}

void test_response_serialization_success() {
    std::cout << "[RUN] test_response_serialization_success..." << std::endl;
    http::Response resp(http::StatusCode::OK);
    resp.headers.set("Date", "Sun, 06 Nov 1994 08:49:37 GMT");
    resp.headers.set("Server", "httpserver/1.0");
    resp.headers.set("Connection", "close");
    resp.set_body("Hello, World!\r\n", "text/plain; charset=utf-8");

    auto head_res = http::serialize_response_head(resp);
    CHECK(head_res.is_ok());
    const std::string& head = head_res.value();

    // Verify status line
    CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    // Verify headers end with double CRLF
    CHECK(head.ends_with("\r\n\r\n"));
    // Verify CRLF termination on lines
    CHECK(head.find("\r\nContent-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    CHECK(head.find("\r\nContent-Length: 15\r\n") != std::string::npos);
    CHECK(head.find("\r\nConnection: close\r\n") != std::string::npos);

    // Full response
    auto full_res = http::serialize_response(resp);
    CHECK(full_res.is_ok());
    const std::string& full = full_res.value();
    CHECK(full.ends_with("Hello, World!\r\n"));
    CHECK(full.size() == head.size() + 15);
    std::cout << "[PASS] test_response_serialization_success" << std::endl;
}

void test_response_serialization_rejection() {
    std::cout << "[RUN] test_response_serialization_rejection..." << std::endl;
    // 1. Invalid status code
    http::Response invalid_status(99, "Bad");
    CHECK(http::serialize_response_head(invalid_status).has_error());

    // 2. Invalid reason phrase (CRLF injection)
    http::Response invalid_reason(200, "OK\r\nInjected: evil");
    CHECK(http::serialize_response_head(invalid_reason).has_error());

    // 3. Invalid header name
    http::Response invalid_header_name(http::StatusCode::OK);
    invalid_header_name.headers.add("Bad Header Name", "value");
    CHECK(http::serialize_response_head(invalid_header_name).has_error());

    // 4. Invalid header value (CRLF injection)
    http::Response invalid_header_val(http::StatusCode::OK);
    invalid_header_val.headers.add("X-Custom", "evil\r\nSet-Cookie: stolen");
    CHECK(http::serialize_response_head(invalid_header_val).has_error());

    std::cout << "[PASS] test_response_serialization_rejection" << std::endl;
}

void test_fixed_length_and_limits() {
    http::Response response;
    response.body = std::string("a\0\xff\r\nb", 6);
    auto wire = test::take(http::serialize_response(response));
    CHECK(wire.find("Content-Length: 6\r\n") != std::string::npos);
    CHECK(wire.substr(wire.find("\r\n\r\n") + 4) == response.body);

    response.headers.set("Content-Length", "6");
    const auto head = test::take(http::serialize_response_head(response));
    CHECK(http::serialize_response(response, {head.size(), 6}));
    CHECK(!http::serialize_response(response, {head.size() - 1, 6}));
    CHECK(!http::serialize_response(response, {head.size(), 5}));
    for (const auto* length : {"5", "7", "-1", "+6", "6,6", " 6", "6x", "", "999999999999999999999999999"}) {
        response.headers.set("Content-Length", length);
        CHECK(!http::serialize_response(response));
    }
    response.headers.set("Content-Length", "6");
    response.headers.add("content-length", "6");
    CHECK(!http::serialize_response(response));
    response.headers.remove("Content-Length");
    response.headers.add("Transfer-Encoding", "chunked");
    CHECK(http::serialize_response(response));
    response.headers.set("Content-Length", "6");
    CHECK(!http::serialize_response(response));

    for (int code : {100, 204, 304}) {
        http::Response no_body(code, "No Body");
        CHECK(http::serialize_response(no_body));
        no_body.body = "x";
        CHECK(!http::serialize_response(no_body));
        no_body.body.clear();
        no_body.headers.set("Content-Length", "42");
        CHECK(static_cast<bool>(http::serialize_response(no_body)) == (code == 304));
    }
    CHECK(!http::serialize_response(http::Response(600, "Invalid")));
    CHECK(!http::serialize_response(http::Response(999, "Invalid")));
    CHECK(http::format_http_date(std::chrono::system_clock::time_point{} - std::chrono::seconds(1)) ==
          "Wed, 31 Dec 1969 23:59:59 GMT");
}

void test_request_serialization_injection() {
    http::Request request;
    CHECK(test::take(http::serialize_request_head(request)).starts_with("GET / HTTP/1.1\r\n"));
    request.method = http::Method::HEAD;
    CHECK(test::take(http::serialize_request_head(request)).starts_with("HEAD / HTTP/1.1\r\n"));
    for (const auto& method : {std::string("GE T"), std::string("GET\r\nX: yes"), std::string("G\0T", 3)}) {
        request.raw_method = method;
        CHECK(!http::serialize_request_head(request));
    }
    request.raw_method.clear();
    for (const auto& target : {std::string(""), std::string("/a b"), std::string("/\r\nX: yes"),
                               std::string("/\0x", 3), std::string("/\x7f")}) {
        request.target = target;
        CHECK(!http::serialize_request_head(request));
    }
    request.target = "/";
    request.version = {2, 0};
    CHECK(!http::serialize_request_head(request));
    request.version = {1, 10};
    CHECK(!http::serialize_request_head(request));
    http::HeaderMap repeated;
    repeated.add("Set-Cookie", "a=1");
    repeated.add("set-cookie", "b=2");
    repeated.add("X-Other", "preserved");
    repeated.set("SET-COOKIE", "c=3");
    CHECK(repeated.get_all("set-cookie").size() == 1);
    CHECK(repeated.get("Set-Cookie") == "c=3");
    CHECK(repeated.get("X-Other") == "preserved");
}

int main() {
    std::cout << "Starting HTTP Serialization Tests..." << std::endl;
    test_http_methods();
    test_http_status_and_reason();
    test_http_version();
    test_header_validation();
    test_header_map_repeated_and_case_insensitive();
    test_http_date_formatting();
    test_response_serialization_success();
    test_response_serialization_rejection();
    test_fixed_length_and_limits();
    test_request_serialization_injection();
    std::cout << "All HTTP Serialization Tests PASSED!" << std::endl;
    return 0;
}
