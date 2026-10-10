#include "http/date.hpp"
#include "http/header.hpp"
#include "http/method.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/serializer.hpp"
#include "http/status.hpp"
#include "http/version.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

void test_http_methods() {
    std::cout << "[RUN] test_http_methods..." << std::endl;
    assert(http::to_string(http::Method::GET) == "GET");
    assert(http::to_string(http::Method::HEAD) == "HEAD");
    assert(http::to_string(http::Method::POST) == "POST");

    assert(http::method_from_string("GET") == http::Method::GET);
    assert(http::method_from_string("HEAD") == http::Method::HEAD);
    assert(!http::method_from_string("INVALID").has_value());

    assert(http::is_safe_method(http::Method::GET));
    assert(http::is_safe_method(http::Method::HEAD));
    assert(!http::is_safe_method(http::Method::POST));

    assert(http::is_idempotent_method(http::Method::GET));
    assert(http::is_idempotent_method(http::Method::PUT));
    assert(!http::is_idempotent_method(http::Method::POST));
    std::cout << "[PASS] test_http_methods" << std::endl;
}

void test_http_status_and_reason() {
    std::cout << "[RUN] test_http_status_and_reason..." << std::endl;
    assert(http::default_reason_phrase(http::StatusCode::OK) == "OK");
    assert(http::default_reason_phrase(http::StatusCode::NotFound) == "NotFound");
    assert(http::default_reason_phrase(http::StatusCode::BadRequest) == "Bad Request");
    assert(http::default_reason_phrase(http::StatusCode::HeaderFieldsTooLarge) == "Request Header Fields Too Large");
    assert(http::default_reason_phrase(http::StatusCode::InternalServerError) == "Internal Server Error");

    assert(http::is_valid_status_code(100));
    assert(http::is_valid_status_code(200));
    assert(http::is_valid_status_code(505));
    assert(http::is_valid_status_code(999));
    assert(!http::is_valid_status_code(99));
    assert(!http::is_valid_status_code(1000));

    assert(http::is_valid_reason_phrase("OK"));
    assert(http::is_valid_reason_phrase("Not Found"));
    assert(!http::is_valid_reason_phrase("Bad\r\nPhrase"));
    assert(!http::is_valid_reason_phrase("Bad\nPhrase"));
    std::cout << "[PASS] test_http_status_and_reason" << std::endl;
}

void test_http_version() {
    std::cout << "[RUN] test_http_version..." << std::endl;
    http::HttpVersion v11 = http::HttpVersion::http_1_1();
    assert(v11.major == 1 && v11.minor == 1);
    assert(v11.to_string() == "HTTP/1.1");
    assert(v11.is_valid());

    http::HttpVersion v10 = http::HttpVersion::http_1_0();
    assert(v10 < v11);
    assert(v10 != v11);
    assert((v11 == http::HttpVersion{1, 1}));
    std::cout << "[PASS] test_http_version" << std::endl;
}

void test_header_validation() {
    std::cout << "[RUN] test_header_validation..." << std::endl;
    // Valid header names
    assert(http::is_valid_header_name("Content-Type"));
    assert(http::is_valid_header_name("Content-Length"));
    assert(http::is_valid_header_name("Host"));
    assert(http::is_valid_header_name("X-Custom_Header.1"));
    assert(http::is_valid_header_name("ETag"));

    // Invalid header names
    assert(!http::is_valid_header_name(""));
    assert(!http::is_valid_header_name("Content Type")); // Space
    assert(!http::is_valid_header_name("Header:1"));     // Colon
    assert(!http::is_valid_header_name("Header\r\n"));   // CRLF
    assert(!http::is_valid_header_name("Header@"));      // @ not tchar
    assert(!http::is_valid_header_name("Header[x]"));    // Brackets

    // Valid header values
    assert(http::is_valid_header_value("text/plain; charset=utf-8"));
    assert(http::is_valid_header_value("12345"));
    assert(http::is_valid_header_value(""));
    assert(http::is_valid_header_value("gzip, deflate"));
    assert(http::is_valid_header_value("foo\tbar"));

    // Invalid header values (CRLF injection prevention)
    assert(!http::is_valid_header_value("value\r\nInjected: evil"));
    assert(!http::is_valid_header_value("value\ninjection"));
    assert(!http::is_valid_header_value("value\r"));
    std::string null_injected = "val";
    null_injected.push_back('\0');
    null_injected += "ue";
    assert(!http::is_valid_header_value(null_injected));
    std::cout << "[PASS] test_header_validation" << std::endl;
}

void test_header_map_repeated_and_case_insensitive() {
    std::cout << "[RUN] test_header_map_repeated_and_case_insensitive..." << std::endl;
    http::HeaderMap map;

    // Test case-insensitive lookup
    map.set("Content-Type", "text/html");
    assert(map.contains("content-type"));
    assert(map.contains("CONTENT-TYPE"));
    assert(map.contains("Content-Type"));
    assert(!map.contains("Content-Length"));

    auto ct = map.get("content-type");
    assert(ct.has_value());
    assert(ct.value() == "text/html");

    // Test repeated fields without joining
    map.add("Set-Cookie", "session_id=123; Path=/");
    map.add("Set-Cookie", "theme=dark; Path=/");
    map.add("set-cookie", "lang=en; Path=/");

    assert(map.size() == 4);
    auto cookies = map.get_all("SET-COOKIE");
    assert(cookies.size() == 3);
    assert(cookies[0] == "session_id=123; Path=/");
    assert(cookies[1] == "theme=dark; Path=/");
    assert(cookies[2] == "lang=en; Path=/");

    // Test set replaces case-insensitively
    map.set("content-type", "application/json");
    assert(map.get("Content-Type").value() == "application/json");
    assert(map.get_all("Content-Type").size() == 1);

    // Test remove
    assert(map.remove("set-cookie"));
    assert(!map.contains("Set-Cookie"));
    assert(map.get_all("Set-Cookie").empty());
    assert(map.size() == 1);

    std::cout << "[PASS] test_header_map_repeated_and_case_insensitive" << std::endl;
}

void test_http_date_formatting() {
    std::cout << "[RUN] test_http_date_formatting..." << std::endl;
    // Fixed timestamp: 784111777 is Sun, 06 Nov 1994 08:49:37 GMT
    auto tp = std::chrono::system_clock::from_time_t(784111777);
    std::string date_str = http::format_http_date(tp);
    assert(date_str == "Sun, 06 Nov 1994 08:49:37 GMT");

    // Current time formatting should produce 29 characters ending in " GMT"
    std::string now_str = http::format_http_date();
    assert(now_str.size() == 29);
    assert(now_str.substr(25) == " GMT");
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
    assert(head_res.is_ok());
    const std::string& head = head_res.value();

    // Verify status line
    assert(head.starts_with("HTTP/1.1 200 OK\r\n"));
    // Verify headers end with double CRLF
    assert(head.ends_with("\r\n\r\n"));
    // Verify CRLF termination on lines
    assert(head.find("\r\nContent-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    assert(head.find("\r\nContent-Length: 15\r\n") != std::string::npos);
    assert(head.find("\r\nConnection: close\r\n") != std::string::npos);

    // Full response
    auto full_res = http::serialize_response(resp);
    assert(full_res.is_ok());
    const std::string& full = full_res.value();
    assert(full.ends_with("Hello, World!\r\n"));
    assert(full.size() == head.size() + 15);
    std::cout << "[PASS] test_response_serialization_success" << std::endl;
}

void test_response_serialization_rejection() {
    std::cout << "[RUN] test_response_serialization_rejection..." << std::endl;
    // 1. Invalid status code
    http::Response invalid_status(99, "Bad");
    assert(http::serialize_response_head(invalid_status).has_error());

    // 2. Invalid reason phrase (CRLF injection)
    http::Response invalid_reason(200, "OK\r\nInjected: evil");
    assert(http::serialize_response_head(invalid_reason).has_error());

    // 3. Invalid header name
    http::Response invalid_header_name(http::StatusCode::OK);
    invalid_header_name.headers.add("Bad Header Name", "value");
    assert(http::serialize_response_head(invalid_header_name).has_error());

    // 4. Invalid header value (CRLF injection)
    http::Response invalid_header_val(http::StatusCode::OK);
    invalid_header_val.headers.add("X-Custom", "evil\r\nSet-Cookie: stolen");
    assert(http::serialize_response_head(invalid_header_val).has_error());

    std::cout << "[PASS] test_response_serialization_rejection" << std::endl;
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
    std::cout << "All HTTP Serialization Tests PASSED!" << std::endl;
    return 0;
}
