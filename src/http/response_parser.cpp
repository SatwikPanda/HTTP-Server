#include "http/response_parser.hpp"

#include "detail/parsing.hpp"
#include "http/framing.hpp"
#include "http/header.hpp"
#include "http/status.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace http {
namespace {

constexpr std::size_t kVersionBytes = 8; // HTTP/x.y

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
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
            if (available > limits_.max_body_bytes - response_.body.size()) {
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
    if (state_ == State::failed) return false;
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
    saw_cr_ = false;

    framing_ = BodyFraming::none;
    body_bytes_remaining_ = 0;
    chunk_size_ = 0;
    chunk_bytes_remaining_ = 0;
    chunk_ext_bytes_ = 0;
    chunk_line_bytes_ = 0;
    chunk_extensions_.clear();
    trailer_bytes_ = 0;
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

    if ((state_ == State::chunk_size || state_ == State::chunk_extension) && byte != '\r' && byte != '\n') {
        if (chunk_line_bytes_ == limits_.max_chunk_line_bytes) return fail(ParseError::chunk_line_too_long);
        ++chunk_line_bytes_;
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
            } else {
                if (chunk_size_ > limits_.max_body_bytes - response_.body.size()) {
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
        if (byte == ';' || byte == ' ' || byte == '\t') {
            if (!saw_hex_digit_) return fail(ParseError::invalid_chunk_size);
            state_ = State::chunk_extension;
            chunk_ext_bytes_ = 1;
            if (chunk_ext_bytes_ > limits_.max_chunk_extension_bytes) return fail(ParseError::chunk_extension_too_large);
            chunk_extensions_.assign(1, byte);
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
            if (!is_valid_chunk_extensions(chunk_extensions_)) return fail(ParseError::invalid_chunk_extension);
            if (chunk_size_ == 0) {
                state_ = State::trailers;
                trailer_line_.clear();
                trailer_bytes_ = 0;
            } else {
                if (chunk_size_ > limits_.max_body_bytes - response_.body.size()) {
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
        if (!is_valid_header_value(std::string_view(&byte, 1))) return fail(ParseError::invalid_chunk_extension);
        chunk_extensions_.push_back(byte);
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
        chunk_line_bytes_ = 0;
        chunk_extensions_.clear();
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
    const auto position = line_.size();
    if (position == limits_.max_request_line_bytes) return fail(ParseError::status_line_too_long);
    constexpr std::string_view version = "HTTP/0.0";
    if (position < kVersionBytes) {
        if ((position == 5 || position == 7) ? !is_digit(byte) : byte != version[position]) {
            return fail(ParseError::malformed_version);
        }
    } else if (position == 8 || position == 12) {
        if (byte != ' ') return fail(ParseError::malformed_status_line);
    } else if (position < 12) {
        if (!is_digit(byte)) return fail(ParseError::malformed_status_line);
    } else if (!is_valid_reason_phrase(std::string_view(&byte, 1))) {
        return fail(ParseError::malformed_status_line);
    }
    line_.push_back(byte);
    return true;
}

bool ResponseParser::finish_line() {
    if (state_ == State::status_line) {
        return finish_status_line();
    }
    if (line_.empty()) return finish_head();
    return finish_field_line();
}

bool ResponseParser::finish_status_line() {
    const std::string_view line = line_;
    if (line.size() < 13) return fail(ParseError::malformed_status_line);
    const HttpVersion parsed_version{
        static_cast<std::uint16_t>(line[5] - '0'),
        static_cast<std::uint16_t>(line[7] - '0')
    };
    if (parsed_version != HttpVersion::http_1_0() && parsed_version != HttpVersion::http_1_1()) {
        return fail(ParseError::unsupported_version);
    }

    const int code = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (!is_valid_status_code(code)) return fail(ParseError::malformed_status_line);
    response_ = Response(code, std::string(line.substr(13)));
    response_.version = parsed_version;
    line_.clear();
    state_ = State::headers;
    return true;
}

bool ResponseParser::finish_field_line() {
    const auto error = detail::parse_field_line(line_, response_.headers, limits_.max_header_count);
    if (error != ParseError::none) return fail(error);
    line_.clear();
    return true;
}

bool ResponseParser::finish_head() {
    const auto rf = select_response_framing(response_, originating_method_);
    if (rf.error != ParseError::none) return fail(rf.error);
    // Check informational responses (100-199)
    if (response_.status_code >= 100 && response_.status_code < 200) {
        informational_responses_.push_back(std::move(response_));
        response_ = Response{};
        state_ = State::status_line;
        line_.clear();
        return true;
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
    const auto error = detail::parse_field_line(trailer_line_, response_.trailers,
                                              limits_.max_trailer_count, &response_.headers);
    if (error != ParseError::none) return fail(error);
    trailer_line_.clear();
    return true;
}

} // namespace http
