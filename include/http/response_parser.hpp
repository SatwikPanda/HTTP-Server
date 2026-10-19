#pragma once

#include "core/error.hpp"
#include "http/framing.hpp"
#include "http/limits.hpp"
#include "http/method.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "http/status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace http {

// Incremental HTTP/1.x response parser for upstream origin responses.
// Operates under the context of the originating request method (e.g. HEAD).
// Bodies are transfer-decoded into a bounded string; Content-Encoding is opaque.
// Completion leaves the next response's bytes unconsumed. Interim heads are
// retained separately under the same cumulative header-byte budget.
class ResponseParser {
public:
    explicit ResponseParser(Method originating_method = Method::GET, RequestLimits limits = {});

    ConsumeResult consume(std::span<const std::byte> bytes);
    ConsumeResult consume(std::string_view bytes);

    // Signals stream closure from peer (EOF).
    // For close-delimited responses, EOF is normal completion.
    // For incomplete fixed-length or chunked responses, marks premature_eof.
    [[nodiscard]] bool finish_on_eof();

    void reset();

    [[nodiscard]] bool complete() const noexcept { return state_ == State::complete; }
    [[nodiscard]] bool failed() const noexcept { return state_ == State::failed; }
    [[nodiscard]] ParseError error() const noexcept { return error_; }
    [[nodiscard]] core::Error error_detail() const;

    // Includes all informational heads and the final head since reset().
    [[nodiscard]] std::size_t head_bytes() const noexcept { return head_bytes_; }
    [[nodiscard]] BodyFraming framing() const noexcept { return framing_; }
    [[nodiscard]] Method originating_method() const noexcept { return originating_method_; }

    [[nodiscard]] const Response& response() const noexcept { return response_; }
    [[nodiscard]] const std::vector<Response>& informational_responses() const noexcept {
        return informational_responses_;
    }

private:
    enum class State {
        status_line,
        headers,
        body_fixed_length,
        chunk_size,
        chunk_extension,
        chunk_data,
        chunk_data_crlf,
        trailers,
        body_close_delimited,
        complete,
        failed
    };

    [[nodiscard]] bool accept_byte(char byte);
    [[nodiscard]] bool accept_status_line_byte(char byte);
    [[nodiscard]] bool finish_line();
    [[nodiscard]] bool finish_status_line();
    [[nodiscard]] bool finish_field_line();
    [[nodiscard]] bool finish_head();
    [[nodiscard]] bool finish_trailer_line();
    bool fail(ParseError error) noexcept;

    Method originating_method_{Method::GET};
    RequestLimits limits_;
    State state_{State::status_line};
    ParseError error_{ParseError::none};
    Response response_;
    std::vector<Response> informational_responses_;
    std::string line_;
    std::string trailer_line_;
    std::size_t head_bytes_{0};
    bool saw_cr_{false};

    // Body & chunk decoding state
    BodyFraming framing_{BodyFraming::none};
    std::uint64_t body_bytes_remaining_{0};
    std::uint64_t chunk_size_{0};
    std::uint64_t chunk_bytes_remaining_{0};
    std::size_t chunk_ext_bytes_{0};
    std::size_t chunk_line_bytes_{0};
    std::string chunk_extensions_;
    std::size_t trailer_bytes_{0};
    bool saw_hex_digit_{false};
    int chunk_crlf_phase_{0};
};

} // namespace http
