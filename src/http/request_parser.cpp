#include "http/request_parser.hpp"

#include "http/framing.hpp"
#include "http/header.hpp"
#include "http/method.hpp"

#include <limits>
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
        case ParseError::ambiguous_framing: return "ambiguous framing: both Transfer-Encoding and Content-Length";
        case ParseError::conflicting_content_length: return "conflicting Content-Length headers";
        case ParseError::invalid_content_length: return "invalid Content-Length header";
        case ParseError::content_length_overflow: return "Content-Length numeric overflow";
        case ParseError::invalid_transfer_encoding: return "invalid Transfer-Encoding header";
        case ParseError::unsupported_transfer_coding: return "unsupported transfer coding";
        case ParseError::invalid_chunk_size: return "invalid chunk size";
        case ParseError::chunk_size_overflow: return "chunk size numeric overflow";
        case ParseError::chunk_extension_too_large: return "chunk extension too large";
        case ParseError::malformed_chunk: return "malformed chunk framing";
        case ParseError::body_too_large: return "request body too large";
        case ParseError::too_many_trailers: return "too many trailer fields";
        case ParseError::trailer_too_large: return "trailer section too large";
        case ParseError::prohibited_trailer: return "prohibited trailer field";
        case ParseError::premature_eof: return "premature end of input stream";
        case ParseError::nonempty_get_head_body: return "nonempty body on GET/HEAD request";
        case ParseError::expectation_failed: return "unsupported Expect header";
        case ParseError::method_not_allowed: return "method not allowed";
        case ParseError::not_implemented: return "feature or method not implemented";
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
        case ParseError::trailer_too_large:
        case ParseError::too_many_trailers:
        case ParseError::chunk_extension_too_large:
            return StatusCode::HeaderFieldsTooLarge;
        case ParseError::body_too_large:
            return StatusCode::PayloadTooLarge;
        case ParseError::expectation_failed:
            return StatusCode::ExpectationFailed;
        case ParseError::method_not_allowed:
            return StatusCode::MethodNotAllowed;
        case ParseError::unsupported_transfer_coding:
        case ParseError::not_implemented:
            return StatusCode::NotImplemented;
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
        if (state_ == State::complete) return {consumed, ParseEvent::complete};
        if (state_ == State::failed) return {consumed, ParseEvent::error};

        if (state_ == State::body_fixed_length) {
            const std::size_t available = bytes.size() - consumed;
            const std::size_t take = std::min(static_cast<std::uint64_t>(available), body_bytes_remaining_);
            const char* ptr = reinterpret_cast<const char*>(bytes.data() + consumed);
            request_.body.append(ptr, take);
            body_bytes_remaining_ -= take;
            consumed += take;
            if (body_bytes_remaining_ == 0) {
                state_ = State::complete;
                return {consumed, ParseEvent::complete};
            }
            continue;
        }

        if (state_ == State::chunk_data) {
            const std::size_t available = bytes.size() - consumed;
            const std::size_t take = std::min(static_cast<std::uint64_t>(available), chunk_bytes_remaining_);
            const char* ptr = reinterpret_cast<const char*>(bytes.data() + consumed);
            request_.body.append(ptr, take);
            chunk_bytes_remaining_ -= take;
            consumed += take;
            if (chunk_bytes_remaining_ == 0) {
                state_ = State::chunk_data_crlf;
                chunk_crlf_phase_ = 0;
            }
            continue;
        }

        const auto byte = static_cast<char>(bytes[consumed]);
        ++consumed;
        if (!accept_byte(byte)) return {consumed, ParseEvent::error};
        if (state_ == State::complete) return {consumed, ParseEvent::complete};
    }
    return {consumed, state_ == State::complete ? ParseEvent::complete : ParseEvent::need_more};
}

bool RequestParser::finish_on_eof() {
    if (state_ == State::complete) return true;
    fail(ParseError::premature_eof);
    return false;
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

    framing_ = ParserFraming::none;
    body_bytes_remaining_ = 0;
    chunk_size_ = 0;
    chunk_bytes_remaining_ = 0;
    chunk_ext_bytes_ = 0;
    trailer_bytes_ = 0;
    trailer_count_ = 0;
    trailer_line_.clear();
    saw_hex_digit_ = false;
    chunk_crlf_phase_ = 0;
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
    if (state_ == State::request_line || state_ == State::headers) {
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

    if (state_ == State::chunk_size) {
        if (saw_cr_) {
            if (byte != '\n') return fail(ParseError::bare_line_ending);
            saw_cr_ = false;
            if (!saw_hex_digit_) return fail(ParseError::invalid_chunk_size);
            if (chunk_size_ == 0) {
                state_ = State::trailers;
                trailer_line_.clear();
                trailer_bytes_ = 0;
                trailer_count_ = 0;
            } else {
                if (request_.body.size() + chunk_size_ > limits_.max_body_bytes) {
                    return fail(ParseError::body_too_large);
                }
                chunk_bytes_remaining_ = chunk_size_;
                state_ = State::chunk_data;
            }
            return true;
        }
        if (byte == '\r') {
            saw_cr_ = true;
            return true;
        }
        if (byte == '\n') return fail(ParseError::bare_line_ending);
        if (byte == ';') {
            if (!saw_hex_digit_) return fail(ParseError::invalid_chunk_size);
            state_ = State::chunk_extension;
            chunk_ext_bytes_ = 0;
            return true;
        }
        if (is_hex_digit(byte)) {
            saw_hex_digit_ = true;
            unsigned val = is_digit(byte) ? static_cast<unsigned>(byte - '0') :
                (byte >= 'a' && byte <= 'f') ? static_cast<unsigned>(10 + byte - 'a') :
                static_cast<unsigned>(10 + byte - 'A');
            if (chunk_size_ > (std::numeric_limits<std::uint64_t>::max() - val) / 16) {
                return fail(ParseError::chunk_size_overflow);
            }
            chunk_size_ = chunk_size_ * 16 + val;
            return true;
        }
        return fail(ParseError::invalid_chunk_size);
    }

    if (state_ == State::chunk_extension) {
        if (saw_cr_) {
            if (byte != '\n') return fail(ParseError::bare_line_ending);
            saw_cr_ = false;
            if (chunk_size_ == 0) {
                state_ = State::trailers;
                trailer_line_.clear();
                trailer_bytes_ = 0;
                trailer_count_ = 0;
            } else {
                if (request_.body.size() + chunk_size_ > limits_.max_body_bytes) {
                    return fail(ParseError::body_too_large);
                }
                chunk_bytes_remaining_ = chunk_size_;
                state_ = State::chunk_data;
            }
            return true;
        }
        if (byte == '\r') {
            saw_cr_ = true;
            return true;
        }
        if (byte == '\n') return fail(ParseError::bare_line_ending);
        ++chunk_ext_bytes_;
        if (chunk_ext_bytes_ > limits_.max_chunk_extension_bytes) {
            return fail(ParseError::chunk_extension_too_large);
        }
        return true;
    }

    if (state_ == State::chunk_data_crlf) {
        if (chunk_crlf_phase_ == 0) {
            if (byte != '\r') return fail(ParseError::malformed_chunk);
            chunk_crlf_phase_ = 1;
            return true;
        }
        if (byte != '\n') return fail(ParseError::malformed_chunk);
        state_ = State::chunk_size;
        chunk_size_ = 0;
        saw_hex_digit_ = false;
        chunk_ext_bytes_ = 0;
        return true;
    }

    if (state_ == State::trailers) {
        ++trailer_bytes_;
        if (trailer_bytes_ > limits_.max_trailer_bytes) {
            return fail(ParseError::trailer_too_large);
        }
        if (saw_cr_) {
            if (byte != '\n') return fail(ParseError::bare_line_ending);
            saw_cr_ = false;
            return finish_trailer_line();
        }
        if (byte == '\r') {
            saw_cr_ = true;
            return true;
        }
        if (byte == '\n') return fail(ParseError::bare_line_ending);
        trailer_line_.push_back(byte);
        return true;
    }

    return fail(ParseError::none);
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

    const auto rf = select_request_framing(request_.headers);
    if (rf.error != ParseError::none) {
        return fail(rf.error);
    }

    if (rf.framing == BodyFraming::none) {
        framing_ = ParserFraming::none;
        state_ = State::complete;
        return true;
    }

    if (rf.framing == BodyFraming::fixed_length) {
        framing_ = ParserFraming::fixed_length;
        if (rf.content_length == 0) {
            state_ = State::complete;
            return true;
        }
        if (rf.content_length > limits_.max_body_bytes) {
            return fail(ParseError::body_too_large);
        }
        body_bytes_remaining_ = rf.content_length;
        request_.body.reserve(static_cast<std::size_t>(rf.content_length));
        state_ = State::body_fixed_length;
        return true;
    }

    if (rf.framing == BodyFraming::chunked) {
        framing_ = ParserFraming::chunked;
        state_ = State::chunk_size;
        chunk_size_ = 0;
        chunk_bytes_remaining_ = 0;
        chunk_ext_bytes_ = 0;
        saw_hex_digit_ = false;
        return true;
    }

    state_ = State::complete;
    return true;
}

bool RequestParser::finish_trailer_line() {
    if (trailer_line_.empty()) {
        state_ = State::complete;
        return true;
    }
    if (trailer_line_.front() == ' ' || trailer_line_.front() == '\t') {
        return fail(ParseError::obsolete_line_folding);
    }
    const std::string_view line = trailer_line_;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return fail(ParseError::malformed_header);
    const auto name = line.substr(0, colon);
    if (!is_valid_header_name(name)) return fail(ParseError::malformed_header);
    const auto value = trim_whitespace(line.substr(colon + 1));
    if (!is_valid_header_value(value)) return fail(ParseError::malformed_header);
    if (is_prohibited_trailer(name)) {
        return fail(ParseError::prohibited_trailer);
    }
    ++trailer_count_;
    if (trailer_count_ > limits_.max_trailer_count) {
        return fail(ParseError::too_many_trailers);
    }
    request_.trailers.add(std::string(name), std::string(value));
    trailer_line_.clear();
    return true;
}

} // namespace http
