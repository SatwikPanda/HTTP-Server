#include "http/response_parser.hpp"

#include "http/framing.hpp"
#include "http/header.hpp"
#include "http/status.hpp"

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

std::string_view trim_whitespace(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

} // namespace

ResponseParser::ResponseParser(Method originating_method, RequestLimits limits)
    : originating_method_(originating_method), limits_(limits) {}

ConsumeResult ResponseParser::consume(std::string_view bytes) {
    return consume(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

ConsumeResult ResponseParser::consume(std::span<const std::byte> bytes) {
    if (state_ == State::complete) return {0, ParseEvent::complete};
    if (state_ == State::failed) return {0, ParseEvent::error};

    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        if (state_ == State::complete) return {consumed, ParseEvent::complete};
        if (state_ == State::failed) return {consumed, ParseEvent::error};

        if (state_ == State::body_close_delimited) {
            const std::size_t available = bytes.size() - consumed;
            if (response_.body.size() + available > limits_.max_body_bytes) {
                fail(ParseError::body_too_large);
                return {consumed, ParseEvent::error};
            }
            const char* ptr = reinterpret_cast<const char*>(bytes.data() + consumed);
            response_.body.append(ptr, available);
            consumed += available;
            continue;
        }

        if (state_ == State::body_fixed_length) {
            const std::size_t available = bytes.size() - consumed;
            const std::size_t take = std::min(static_cast<std::uint64_t>(available), body_bytes_remaining_);
            const char* ptr = reinterpret_cast<const char*>(bytes.data() + consumed);
            response_.body.append(ptr, take);
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
            response_.body.append(ptr, take);
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

bool ResponseParser::finish_on_eof() {
    if (state_ == State::complete) return true;
    if (state_ == State::body_close_delimited) {
        state_ = State::complete;
        return true;
    }
    fail(ParseError::premature_eof);
    return false;
}

void ResponseParser::reset() {
    state_ = State::status_line;
    error_ = ParseError::none;
    response_ = Response{};
    informational_responses_.clear();
    line_.clear();
    trailer_line_.clear();
    head_bytes_ = 0;
    status_line_segment_ = 0;
    segment_bytes_ = 0;
    saw_cr_ = false;

    framing_ = BodyFraming::none;
    body_bytes_remaining_ = 0;
    chunk_size_ = 0;
    chunk_bytes_remaining_ = 0;
    chunk_ext_bytes_ = 0;
    trailer_bytes_ = 0;
    trailer_count_ = 0;
    saw_hex_digit_ = false;
    chunk_crlf_phase_ = 0;
}

core::Error ResponseParser::error_detail() const {
    if (error_ == ParseError::none) return {};
    return core::make_error(core::ErrorCategory::invalid_argument, "HTTP response parser", 0,
                            std::string(to_string(error_)));
}

bool ResponseParser::fail(ParseError error) noexcept {
    error_ = error;
    state_ = State::failed;
    return false;
}

bool ResponseParser::accept_byte(char byte) {
    if (state_ == State::status_line || state_ == State::headers) {
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

        if (state_ == State::status_line) return accept_status_line_byte(byte);
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
                if (response_.body.size() + chunk_size_ > limits_.max_body_bytes) {
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
                if (response_.body.size() + chunk_size_ > limits_.max_body_bytes) {
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

bool ResponseParser::accept_status_line_byte(char byte) {
    if (byte == ' ') {
        if (status_line_segment_ == 0) {
            if (segment_bytes_ != kVersionBytes) return fail(ParseError::malformed_version);
            status_line_segment_ = 1;
            segment_bytes_ = 0;
        } else if (status_line_segment_ == 1) {
            if (segment_bytes_ != 3) return fail(ParseError::malformed_request_line);
            status_line_segment_ = 2;
            segment_bytes_ = 0;
        } else {
            // Space in reason phrase is allowed
            ++segment_bytes_;
        }
    } else {
        if (status_line_segment_ == 1 && !is_digit(byte)) {
            return fail(ParseError::malformed_request_line);
        }
        ++segment_bytes_;
    }

    if (line_.size() == limits_.max_request_line_bytes) return fail(ParseError::request_line_too_long);
    line_.push_back(byte);
    return true;
}

bool ResponseParser::finish_line() {
    if (state_ == State::status_line) {
        if (line_.empty()) return true; // ignore leading empty lines
        return finish_status_line();
    }
    if (line_.empty()) return finish_head();
    return finish_field_line();
}

bool ResponseParser::finish_status_line() {
    const std::string_view line = line_;
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos) return fail(ParseError::malformed_request_line);

    const auto version = line.substr(0, first_space);
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

    const auto second_space = line.find(' ', first_space + 1);
    std::string_view status_str;
    std::string_view reason_str;

    if (second_space == std::string_view::npos) {
        status_str = line.substr(first_space + 1);
    } else {
        status_str = line.substr(first_space + 1, second_space - first_space - 1);
        reason_str = line.substr(second_space + 1);
    }

    if (status_str.size() != 3 || !is_digit(status_str[0]) ||
        !is_digit(status_str[1]) || !is_digit(status_str[2])) {
        return fail(ParseError::malformed_request_line);
    }

    int code = (status_str[0] - '0') * 100 + (status_str[1] - '0') * 10 + (status_str[2] - '0');
    if (code < 100 || code > 599) return fail(ParseError::malformed_request_line);

    response_.version = parsed_version;
    response_.set_status(code, std::string(reason_str));
    line_.clear();
    state_ = State::headers;
    return true;
}

bool ResponseParser::finish_field_line() {
    if (line_.front() == ' ' || line_.front() == '\t') {
        return fail(ParseError::obsolete_line_folding);
    }

    const std::string_view line = line_;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return fail(ParseError::malformed_header);

    const auto name = line.substr(0, colon);
    if (!is_valid_header_name(name)) return fail(ParseError::malformed_header);

    const auto value = trim_whitespace(line.substr(colon + 1));
    if (!is_valid_header_value(value)) return fail(ParseError::malformed_header);

    if (response_.headers.size() == limits_.max_header_count) return fail(ParseError::too_many_headers);
    response_.headers.add(std::string(name), std::string(value));
    line_.clear();
    return true;
}

bool ResponseParser::finish_head() {
    // Check informational responses (100-199)
    if (response_.status_code >= 100 && response_.status_code < 200) {
        if (response_.status_code == 101) {
            // RFC 9112: protocol switching rejected in proxy
            return fail(ParseError::not_implemented);
        }
        informational_responses_.push_back(std::move(response_));
        response_ = Response{};
        state_ = State::status_line;
        status_line_segment_ = 0;
        segment_bytes_ = 0;
        head_bytes_ = 0;
        line_.clear();
        return true;
    }

    const auto rf = select_response_framing(response_, originating_method_);
    if (rf.error != ParseError::none) {
        return fail(rf.error);
    }

    framing_ = rf.framing;
    if (framing_ == BodyFraming::none) {
        state_ = State::complete;
        return true;
    }

    if (framing_ == BodyFraming::fixed_length) {
        if (rf.content_length == 0) {
            state_ = State::complete;
            return true;
        }
        if (rf.content_length > limits_.max_body_bytes) {
            return fail(ParseError::body_too_large);
        }
        body_bytes_remaining_ = rf.content_length;
        response_.body.reserve(static_cast<std::size_t>(rf.content_length));
        state_ = State::body_fixed_length;
        return true;
    }

    if (framing_ == BodyFraming::chunked) {
        state_ = State::chunk_size;
        chunk_size_ = 0;
        chunk_bytes_remaining_ = 0;
        chunk_ext_bytes_ = 0;
        saw_hex_digit_ = false;
        return true;
    }

    if (framing_ == BodyFraming::close_delimited) {
        state_ = State::body_close_delimited;
        return true;
    }

    state_ = State::complete;
    return true;
}

bool ResponseParser::finish_trailer_line() {
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
    response_.trailers.add(std::string(name), std::string(value));
    trailer_line_.clear();
    return true;
}

} // namespace http
