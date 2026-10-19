#include "http/framing.hpp"

#include "http/header.hpp"
#include "http/method.hpp"

#include <cctype>
#include <limits>
#include <string_view>
#include <vector>

namespace http {
namespace {

std::string_view trim_ws(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::vector<std::string_view> split_comma_tokens(std::string_view text) {
    std::vector<std::string_view> tokens;
    while (!text.empty()) {
        const auto comma = text.find(',');
        if (comma == std::string_view::npos) {
            auto token = trim_ws(text);
            if (!token.empty()) tokens.push_back(token);
            break;
        }
        auto token = trim_ws(text.substr(0, comma));
        if (!token.empty()) tokens.push_back(token);
        text = text.substr(comma + 1);
    }
    return tokens;
}

bool ascii_case_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view to_string(BodyFraming framing) noexcept {
    switch (framing) {
        case BodyFraming::none: return "none";
        case BodyFraming::fixed_length: return "fixed-length";
        case BodyFraming::chunked: return "chunked";
        case BodyFraming::close_delimited: return "close-delimited";
    }
    return "unknown";
}

RequestFraming select_request_framing(const HeaderMap& headers) noexcept {
    const bool has_te = headers.contains("Transfer-Encoding");
    const bool has_cl = headers.contains("Content-Length");

    // RFC 9112 section 6.1: A sender MUST NOT send both Transfer-Encoding and Content-Length.
    if (has_te && has_cl) {
        return {BodyFraming::none, 0, ParseError::ambiguous_framing};
    }

    if (has_te) {
        std::vector<std::string_view> codings;
        for (const auto val : headers.get_all("Transfer-Encoding")) {
            for (const auto token : split_comma_tokens(val)) {
                codings.push_back(token);
            }
        }

        if (codings.empty()) {
            return {BodyFraming::none, 0, ParseError::invalid_transfer_encoding};
        }

        const auto final_coding = codings.back();
        if (!ascii_case_equal(final_coding, "chunked")) {
            // Check if codings are unsupported (e.g. gzip alone without chunked)
            // or an invalid sequence (e.g. chunked, gzip).
            bool saw_chunked = false;
            for (const auto c : codings) {
                if (ascii_case_equal(c, "chunked")) saw_chunked = true;
            }
            if (saw_chunked) {
                return {BodyFraming::none, 0, ParseError::invalid_transfer_encoding};
            }
            return {BodyFraming::none, 0, ParseError::unsupported_transfer_coding};
        }

        return {BodyFraming::chunked, 0, ParseError::none};
    }

    if (has_cl) {
        std::vector<std::string_view> values;
        for (const auto val : headers.get_all("Content-Length")) {
            for (const auto token : split_comma_tokens(val)) {
                values.push_back(token);
            }
        }

        if (values.empty()) {
            return {BodyFraming::none, 0, ParseError::invalid_content_length};
        }

        std::uint64_t parsed_length = 0;
        bool first = true;

        for (const auto token : values) {
            if (token.empty()) {
                return {BodyFraming::none, 0, ParseError::invalid_content_length};
            }

            std::uint64_t current = 0;
            for (const char c : token) {
                if (c < '0' || c > '9') {
                    return {BodyFraming::none, 0, ParseError::invalid_content_length};
                }
                const auto digit = static_cast<std::uint64_t>(c - '0');
                if (current > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                    return {BodyFraming::none, 0, ParseError::content_length_overflow};
                }
                current = current * 10 + digit;
            }

            if (first) {
                parsed_length = current;
                first = false;
            } else if (current != parsed_length) {
                return {BodyFraming::none, 0, ParseError::conflicting_content_length};
            }
        }

        return {BodyFraming::fixed_length, parsed_length, ParseError::none};
    }

    return {BodyFraming::none, 0, ParseError::none};
}

bool is_prohibited_trailer(std::string_view name) noexcept {
    static constexpr std::string_view kProhibited[] = {
        "transfer-encoding",
        "content-length",
        "host",
        "authorization",
        "proxy-authenticate",
        "proxy-authorization",
        "www-authenticate",
        "cookie",
        "set-cookie",
        "expect",
        "max-forwards",
        "te",
        "if-match",
        "if-none-match",
        "if-modified-since",
        "if-unmodified-since",
        "if-range",
        "range",
        "trailer",
        "content-encoding",
        "content-type",
        "content-range"
    };

    for (const auto prohibited : kProhibited) {
        if (ascii_case_equal(name, prohibited)) return true;
    }
    return false;
}

StatusCode check_request_restrictions(const Request& request) noexcept {
    if (request.headers.contains("Expect")) {
        return StatusCode::ExpectationFailed; // 417
    }

    if (request.headers.contains("Upgrade")) {
        return StatusCode::BadRequest; // 400
    }

    if (request.method == Method::CONNECT) {
        return StatusCode::MethodNotAllowed; // 405
    }

    if (request.method == Method::UNKNOWN) {
        return StatusCode::NotImplemented; // 501
    }

    if (request.method != Method::GET && request.method != Method::HEAD) {
        return StatusCode::MethodNotAllowed; // 405
    }

    if ((request.method == Method::GET || request.method == Method::HEAD) && !request.body.empty()) {
        return StatusCode::BadRequest; // 400
    }

    return StatusCode::OK;
}

} // namespace http
