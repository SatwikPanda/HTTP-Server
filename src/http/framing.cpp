#include "http/framing.hpp"

#include "http/header.hpp"
#include "http/method.hpp"

#include <limits>
#include <string_view>

namespace http {
namespace {

std::string_view trim_ws(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

RequestFraming select_framing_fields(const HeaderMap& headers) noexcept {
    const bool has_te = headers.contains("Transfer-Encoding");
    const bool has_cl = headers.contains("Content-Length");
    if (has_te && has_cl) return {BodyFraming::none, 0, ParseError::ambiguous_framing};

    if (has_te) {
        std::size_t count = 0;
        bool chunked = false;
        bool unsupported = false;
        for (const auto& field : headers) {
            if (!header_name_equals(field.name, "Transfer-Encoding")) continue;
            std::string_view rest = field.value;
            do {
                const auto comma = rest.find(',');
                const auto coding = trim_ws(rest.substr(0, comma));
                // This project supports chunked only, with no transfer parameters.
                // Do not silently discard another coding or an empty list member.
                if (!is_valid_header_name(coding) || chunked) {
                    return {BodyFraming::none, 0, ParseError::invalid_transfer_encoding};
                }
                ++count;
                chunked = header_name_equals(coding, "chunked");
                unsupported = unsupported || !chunked;
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            } while (true);
        }
        if (unsupported) return {BodyFraming::none, 0, ParseError::unsupported_transfer_coding};
        if (count != 1 || !chunked) return {BodyFraming::none, 0, ParseError::invalid_transfer_encoding};
        return {BodyFraming::chunked, 0, ParseError::none};
    }

    if (has_cl) {
        bool first = true;
        std::uint64_t length = 0;
        for (const auto& field : headers) {
            if (!header_name_equals(field.name, "Content-Length")) continue;
            std::string_view rest = field.value;
            do {
                const auto comma = rest.find(',');
                const auto token = trim_ws(rest.substr(0, comma));
                if (token.empty()) return {BodyFraming::none, 0, ParseError::invalid_content_length};
                std::uint64_t current = 0;
                for (char c : token) {
                    if (c < '0' || c > '9') return {BodyFraming::none, 0, ParseError::invalid_content_length};
                    const auto digit = static_cast<std::uint64_t>(c - '0');
                    if (current > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                        return {BodyFraming::none, 0, ParseError::content_length_overflow};
                    }
                    current = current * 10 + digit;
                }
                if (!first && current != length) {
                    return {BodyFraming::none, 0, ParseError::conflicting_content_length};
                }
                first = false;
                length = current;
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            } while (true);
        }
        return {BodyFraming::fixed_length, length, ParseError::none};
    }
    return {};
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
    return select_framing_fields(headers);
}

RequestFraming select_response_framing(const Response& response, Method originating_method) noexcept {
    if (response.status_code == 101 ||
        (originating_method == Method::CONNECT && response.status_code >= 200 && response.status_code < 300)) {
        return {BodyFraming::none, 0, ParseError::not_implemented};
    }
    const auto fields = select_framing_fields(response.headers);
    if (fields.error != ParseError::none) return fields;
    const bool informational = response.status_code >= 100 && response.status_code < 200;
    if ((informational || response.status_code == 204) && fields.framing != BodyFraming::none) {
        return {BodyFraming::none, 0, ParseError::ambiguous_framing};
    }
    if (response.version == HttpVersion::http_1_0() && response.headers.contains("Transfer-Encoding")) {
        return {BodyFraming::none, 0, ParseError::invalid_transfer_encoding};
    }
    if (originating_method == Method::HEAD || informational ||
        response.status_code == 204 || response.status_code == 304) return {};
    if (fields.framing != BodyFraming::none) return fields;
    return {BodyFraming::close_delimited, 0, ParseError::none};
}

bool is_prohibited_trailer(std::string_view name, const HeaderMap* head) noexcept {
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
        "content-range", "connection", "keep-alive", "proxy-connection", "upgrade",
        "cache-control", "age", "date", "expires", "vary", "location", "retry-after"
    };

    for (const auto prohibited : kProhibited) {
        if (header_name_equals(name, prohibited)) return true;
    }
    if (head) {
        for (const auto& field : *head) {
            if (!header_name_equals(field.name, "Connection")) continue;
            std::string_view rest = field.value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                if (header_name_equals(name, trim_ws(rest.substr(0, comma)))) return true;
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            }
        }
    }
    return false;
}

bool is_valid_chunk_extensions(std::string_view text) noexcept {
    const auto skip_ws = [&] {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    };
    const auto token = [&] {
        const auto initial = text.size();
        while (!text.empty() && is_valid_header_name(text.substr(0, 1))) text.remove_prefix(1);
        return text.size() != initial;
    };
    while (!text.empty()) {
        skip_ws();
        if (text.empty() || text.front() != ';') return false;
        text.remove_prefix(1);
        skip_ws();
        if (!token()) return false;
        if (text.empty()) return true;
        skip_ws();
        if (text.empty()) return false;
        if (text.front() != '=') continue;
        text.remove_prefix(1);
        skip_ws();
        if (text.empty()) return false;
        if (text.front() != '"') {
            if (!token()) return false;
            continue;
        }
        text.remove_prefix(1);
        bool closed = false;
        while (!text.empty()) {
            unsigned char c = static_cast<unsigned char>(text.front());
            text.remove_prefix(1);
            if (c == '"') { closed = true; break; }
            if (c == '\\') {
                if (text.empty()) return false;
                c = static_cast<unsigned char>(text.front());
                text.remove_prefix(1);
            }
            if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
        }
        if (!closed) return false;
    }
    return true;
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
