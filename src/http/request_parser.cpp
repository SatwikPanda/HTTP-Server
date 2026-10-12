#include "http/request_parser.hpp"

#include "http/header.hpp"
#include "http/method.hpp"

#include <string>
#include <utility>

namespace http {
namespace {

constexpr std::string_view kVersionPrefix = "HTTP/";
constexpr std::size_t kVersionBytes = 8; // HTTP/x.y

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_alnum(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_token_char(char c) noexcept {
    return is_valid_header_name(std::string_view(&c, 1));
}

// Visible ASCII. A fragment ('#') is never part of a request target.
bool is_target_char(char c) noexcept {
    return c > 0x20 && c < 0x7F && c != '#';
}

std::string_view trim_whitespace(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool is_valid_port(std::string_view port) noexcept {
    if (port.empty() || port.size() > 5) return false;
    unsigned value = 0;
    for (char c : port) {
        if (!is_digit(c)) return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value <= 65535;
}

// uri-host [":" port], limited to an IP literal, an IPv4 address, or a DNS-style
// reg-name. Sub-delimiters such as ',' are legal in a generic reg-name but never
// in a server name, and accepting them would let "a,b" pass as one host.
bool is_valid_host(std::string_view host) noexcept {
    if (host.empty()) return false;

    std::string_view rest;
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string_view::npos || close == 1) return false;
        bool saw_colon = false;
        for (char c : host.substr(1, close - 1)) {
            if (c == ':') {
                saw_colon = true;
            } else if (!is_hex_digit(c) && c != '.') {
                return false;
            }
        }
        if (!saw_colon) return false;
        rest = host.substr(close + 1);
    } else {
        const auto colon = host.find(':');
        const auto name = host.substr(0, colon);
        if (name.empty()) return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            if (c == '%') {
                if (i + 2 >= name.size() || !is_hex_digit(name[i + 1]) || !is_hex_digit(name[i + 2])) {
                    return false;
                }
                i += 2;
            } else if (!is_alnum(c) && c != '-' && c != '.' && c != '_' && c != '~') {
                return false;
            }
        }
        rest = colon == std::string_view::npos ? std::string_view{} : host.substr(colon);
    }

    if (rest.empty()) return true;
    return rest.front() == ':' && is_valid_port(rest.substr(1));
}

} // namespace

std::string_view to_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::none: return "none";
        case ParseError::malformed_request_line: return "malformed request line";
        case ParseError::invalid_method: return "invalid method";
        case ParseError::invalid_target: return "invalid request target";
        case ParseError::malformed_version: return "malformed HTTP version";
        case ParseError::unsupported_version: return "unsupported HTTP version";
        case ParseError::bare_line_ending: return "line not terminated by CRLF";
        case ParseError::malformed_header: return "malformed header field";
        case ParseError::obsolete_line_folding: return "obsolete line folding";
        case ParseError::missing_host: return "missing Host field";
        case ParseError::duplicate_host: return "repeated Host field";
        case ParseError::invalid_host: return "invalid Host field";
        case ParseError::request_line_too_long: return "request line too long";
        case ParseError::target_too_long: return "request target too long";
        case ParseError::header_too_large: return "header section too large";
        case ParseError::too_many_headers: return "too many header fields";
    }
    return "unknown";
}

StatusCode status_for(ParseError error) noexcept {
    switch (error) {
        case ParseError::none:
            return StatusCode::OK;
        case ParseError::unsupported_version:
            return StatusCode::HttpVersionNotSupported;
        case ParseError::target_too_long:
            return StatusCode::UriTooLong;
        case ParseError::header_too_large:
        case ParseError::too_many_headers:
            return StatusCode::HeaderFieldsTooLarge;
        default:
            return StatusCode::BadRequest;
    }
}

RequestParser::RequestParser(RequestLimits limits) : limits_(limits) {}

ConsumeResult RequestParser::consume(std::string_view bytes) {
    return consume(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

ConsumeResult RequestParser::consume(std::span<const std::byte> bytes) {
    if (state_ == State::complete) return {0, ParseEvent::complete};
    if (state_ == State::failed) return {0, ParseEvent::error};

    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        const auto byte = static_cast<char>(bytes[consumed]);
        ++consumed;
        if (!accept_byte(byte)) return {consumed, ParseEvent::error};
        if (state_ == State::complete) return {consumed, ParseEvent::complete};
    }
    return {consumed, ParseEvent::need_more};
}

void RequestParser::reset() {
    state_ = State::request_line;
    error_ = ParseError::none;
    request_ = Request{};
    line_.clear();
    head_bytes_ = 0;
    request_line_segment_ = 0;
    segment_bytes_ = 0;
    saw_cr_ = false;
}

core::Error RequestParser::error_detail() const {
    if (error_ == ParseError::none) return {};
    return core::make_error(core::ErrorCategory::invalid_argument, "HTTP request parser", 0,
                            std::string(to_string(error_)));
}

bool RequestParser::fail(ParseError error) noexcept {
    error_ = error;
    state_ = State::failed;
    return false;
}

bool RequestParser::accept_byte(char byte) {
    // Checked before the byte is kept, so a head of exactly the limit completes.
    if (head_bytes_ == limits_.max_header_bytes) return fail(ParseError::header_too_large);
    ++head_bytes_;

    if (saw_cr_) {
        if (byte != '\n') return fail(ParseError::bare_line_ending);
        saw_cr_ = false;
        return finish_line();
    }
    if (byte == '\r') {
        saw_cr_ = true;
        return true;
    }
    if (byte == '\n') return fail(ParseError::bare_line_ending);

    if (state_ == State::request_line) return accept_request_line_byte(byte);
    line_.push_back(byte);
    return true;
}

// Validates the request line as it streams in, so garbage and oversized targets
// are rejected without waiting for a line ending that may never come.
bool RequestParser::accept_request_line_byte(char byte) {
    if (byte == ' ') {
        // Exactly one space separates the three parts.
        if (segment_bytes_ == 0 || request_line_segment_ == 2) {
            return fail(ParseError::malformed_request_line);
        }
        ++request_line_segment_;
        segment_bytes_ = 0;
    } else {
        switch (request_line_segment_) {
            case 0:
                if (!is_token_char(byte)) return fail(ParseError::invalid_method);
                break;
            case 1:
                if (!is_target_char(byte)) return fail(ParseError::invalid_target);
                if (segment_bytes_ == limits_.max_target_bytes) return fail(ParseError::target_too_long);
                break;
            default:
                if (segment_bytes_ == kVersionBytes) return fail(ParseError::malformed_version);
                break;
        }
        ++segment_bytes_;
    }

    if (line_.size() == limits_.max_request_line_bytes) return fail(ParseError::request_line_too_long);
    line_.push_back(byte);
    return true;
}

bool RequestParser::finish_line() {
    if (state_ == State::request_line) {
        // RFC 9112 section 2.2: ignore empty lines received before the request line.
        // They still count toward the header byte limit.
        if (line_.empty()) return true;
        return finish_request_line();
    }
    if (line_.empty()) return finish_head();
    return finish_field_line();
}

bool RequestParser::finish_request_line() {
    if (request_line_segment_ != 2 || segment_bytes_ == 0) {
        return fail(ParseError::malformed_request_line);
    }

    const std::string_view line = line_;
    const auto first_space = line.find(' ');
    const auto second_space = line.find(' ', first_space + 1);
    const auto method = line.substr(0, first_space);
    const auto target = line.substr(first_space + 1, second_space - first_space - 1);
    const auto version = line.substr(second_space + 1);

    if (version.size() != kVersionBytes || !version.starts_with(kVersionPrefix) ||
        !is_digit(version[5]) || version[6] != '.' || !is_digit(version[7])) {
        return fail(ParseError::malformed_version);
    }
    const HttpVersion parsed_version{
        static_cast<std::uint16_t>(version[5] - '0'),
        static_cast<std::uint16_t>(version[7] - '0')
    };
    if (parsed_version != HttpVersion::http_1_0() && parsed_version != HttpVersion::http_1_1()) {
        return fail(ParseError::unsupported_version);
    }

    // Unknown methods are kept here; method policy is applied by the application.
    request_.raw_method = std::string(method);
    request_.method = method_from_string(method).value_or(Method::UNKNOWN);
    request_.target = std::string(target);
    request_.version = parsed_version;
    line_.clear();
    state_ = State::headers;
    return true;
}

bool RequestParser::finish_field_line() {
    // A field line starting with whitespace continues the previous one
    // (obs-fold). RFC 9112 section 5.2 lets a server reject it.
    if (line_.front() == ' ' || line_.front() == '\t') {
        return fail(ParseError::obsolete_line_folding);
    }

    const std::string_view line = line_;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return fail(ParseError::malformed_header);

    // No whitespace is allowed between the name and the colon.
    const auto name = line.substr(0, colon);
    if (!is_valid_header_name(name)) return fail(ParseError::malformed_header);

    const auto value = trim_whitespace(line.substr(colon + 1));
    if (!is_valid_header_value(value)) return fail(ParseError::malformed_header);

    if (request_.headers.size() == limits_.max_header_count) return fail(ParseError::too_many_headers);
    request_.headers.add(std::string(name), std::string(value));
    line_.clear();
    return true;
}

bool RequestParser::finish_head() {
    const auto hosts = request_.headers.get_all("Host");
    if (hosts.size() > 1) return fail(ParseError::duplicate_host);
    if (hosts.empty()) {
        // HTTP/1.0 predates the requirement.
        if (request_.version == HttpVersion::http_1_1()) return fail(ParseError::missing_host);
    } else if (!is_valid_host(hosts.front())) {
        return fail(ParseError::invalid_host);
    }
    state_ = State::complete;
    return true;
}

} // namespace http
