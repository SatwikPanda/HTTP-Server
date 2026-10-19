#pragma once

#include "core/error.hpp"
#include "http/limits.hpp"
#include "http/request.hpp"
#include "http/status.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace http {

enum class ParseError {
    none,
    malformed_request_line,
    invalid_method,
    invalid_target,
    malformed_version,
    unsupported_version,
    bare_line_ending, // CR or LF used alone instead of CRLF
    malformed_header,
    obsolete_line_folding,
    missing_host,
    duplicate_host,
    invalid_host,
    request_line_too_long,
    target_too_long,
    header_too_large,
    too_many_headers,
    ambiguous_framing,
    conflicting_content_length,
    invalid_content_length,
    content_length_overflow,
    invalid_transfer_encoding,
    unsupported_transfer_coding,
    invalid_chunk_size,
    chunk_size_overflow,
    chunk_extension_too_large,
    malformed_chunk,
    body_too_large,
    too_many_trailers,
    trailer_too_large,
    prohibited_trailer,
    premature_eof,
    nonempty_get_head_body,
    expectation_failed,
    method_not_allowed,
    not_implemented
};

[[nodiscard]] std::string_view to_string(ParseError error) noexcept;

// The response status for a rejected request: 505 for unsupported version,
// 414 for overlong target, 431 for header/trailer limits, 413 for body limit,
// 417 for Expect, 405 for disallowed methods, 501 for unimplemented methods or codings,
// 400 otherwise.
[[nodiscard]] StatusCode status_for(ParseError error) noexcept;

enum class ParseEvent {
    need_more,
    complete,
    error
};

struct ConsumeResult {
    std::size_t consumed{0};
    ParseEvent event{ParseEvent::need_more};
};

enum class ParserFraming {
    none,
    fixed_length,
    chunked
};

// Incremental HTTP/1.x request parser. Bytes may arrive split at any
// position. It consumes up to the end of one request (head + body) and leaves
// later bytes (a pipelined request) unconsumed. Once complete
// or failed it consumes nothing until reset(). After an error the connection is
// out of sync and must be closed.
class RequestParser {
public:
    explicit RequestParser(RequestLimits limits = {});

    ConsumeResult consume(std::span<const std::byte> bytes);
    ConsumeResult consume(std::string_view bytes);

    // Signals stream closure from peer. If parsing is not complete, marks premature_eof.
    [[nodiscard]] bool finish_on_eof();

    // Prepares for the next request on the same connection; limits are kept.
    void reset();

    [[nodiscard]] bool complete() const noexcept { return state_ == State::complete; }
    [[nodiscard]] bool failed() const noexcept { return state_ == State::failed; }
    [[nodiscard]] ParseError error() const noexcept { return error_; }
    [[nodiscard]] core::Error error_detail() const;

    // Bytes consumed for the current head so far.
    [[nodiscard]] std::size_t head_bytes() const noexcept { return head_bytes_; }
    [[nodiscard]] ParserFraming framing() const noexcept { return framing_; }

    // Valid once complete(); fields keep their order, case, and repetitions.
    [[nodiscard]] const Request& request() const noexcept { return request_; }
    [[nodiscard]] const RequestLimits& limits() const noexcept { return limits_; }

private:
    enum class State {
        request_line,
        headers,
        body_fixed_length,
        chunk_size,
        chunk_extension,
        chunk_data,
        chunk_data_crlf,
        trailers,
        complete,
        failed
    };

    [[nodiscard]] bool accept_byte(char byte);
    [[nodiscard]] bool accept_request_line_byte(char byte);
    [[nodiscard]] bool finish_line();
    [[nodiscard]] bool finish_request_line();
    [[nodiscard]] bool finish_field_line();
    [[nodiscard]] bool finish_head();
    [[nodiscard]] bool finish_trailer_line();
    bool fail(ParseError error) noexcept;

    RequestLimits limits_;
    State state_{State::request_line};
    ParseError error_{ParseError::none};
    Request request_;
    std::string line_;
    std::string trailer_line_;
    std::size_t head_bytes_{0};
    std::size_t request_line_segment_{0}; // 0 method, 1 target, 2 version
    std::size_t segment_bytes_{0};
    bool saw_cr_{false};

    // Body & chunk decoding state
    ParserFraming framing_{ParserFraming::none};
    std::uint64_t body_bytes_remaining_{0};
    std::uint64_t chunk_size_{0};
    std::uint64_t chunk_bytes_remaining_{0};
    std::size_t chunk_ext_bytes_{0};
    std::size_t trailer_bytes_{0};
    std::size_t trailer_count_{0};
    bool saw_hex_digit_{false};
    int chunk_crlf_phase_{0};
};

} // namespace http
