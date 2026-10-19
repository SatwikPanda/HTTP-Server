#include "http/serializer.hpp"

#include <array>
#include <charconv>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace http {
namespace {

core::Error invalid(std::string_view operation, std::string message) {
    return core::make_error(core::ErrorCategory::invalid_argument,
                            std::string(operation), 0, std::move(message));
}

core::Result<std::optional<std::size_t>> content_length(
    const HeaderMap& headers, std::string_view operation, bool allow_te = false
) {
    std::optional<std::size_t> length;
    for (const auto& field : headers) {
        if (!is_valid_header_name(field.name) || !is_valid_header_value(field.value)) {
            return invalid(operation, "Invalid header name or value");
        }
        if (!allow_te && header_name_equals(field.name, "Transfer-Encoding")) {
            return invalid(operation, "Transfer-Encoding is not supported by the fixed-length serializer");
        }
        if (!header_name_equals(field.name, "Content-Length")) continue;
        if (length || field.value.empty()) {
            return invalid(operation, "Empty or repeated Content-Length");
        }
        for (char c : field.value) {
            if (c < '0' || c > '9') return invalid(operation, "Invalid Content-Length");
        }
        std::size_t parsed{};
        const auto result = std::from_chars(field.value.data(),
                                            field.value.data() + field.value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != field.value.data() + field.value.size()) {
            return invalid(operation, "Content-Length overflow");
        }
        length = parsed;
    }
    return length;
}

bool append(std::string& output, std::string_view text, std::size_t limit) {
    if (output.size() > limit || text.size() > limit - output.size()) return false;
    output.append(text);
    return true;
}

core::Result<std::string> serialize_head(
    std::string_view start_line, const HeaderMap& headers,
    SerializationLimits limits, std::string_view operation
) {
    std::string output;
    if (!append(output, start_line, limits.max_head_bytes) ||
        !append(output, "\r\n", limits.max_head_bytes)) {
        return invalid(operation, "Start line exceeds output head limit");
    }
    for (const auto& field : headers) {
        if (!append(output, field.name, limits.max_head_bytes) ||
            !append(output, ": ", limits.max_head_bytes) ||
            !append(output, field.value, limits.max_head_bytes) ||
            !append(output, "\r\n", limits.max_head_bytes)) {
            return invalid(operation, "Headers exceed output head limit");
        }
    }
    if (!append(output, "\r\n", limits.max_head_bytes)) {
        return invalid(operation, "Headers exceed output head limit");
    }
    return output;
}

core::Result<std::string> response_head(
    const Response& response, const HeaderMap& headers, SerializationLimits limits
) {
    constexpr std::string_view operation = "serialize_response_head";
    if (!response.version.is_valid()) return invalid(operation, "Unsupported HTTP version");
    if (!is_valid_status_code(response.status_code)) return invalid(operation, "Invalid HTTP status code");
    if (response.reason.size() > limits.max_head_bytes || !is_valid_reason_phrase(response.reason)) {
        return invalid(operation, "Invalid or oversized reason phrase");
    }
    if (response.status_code == 101) return invalid(operation, "Protocol switching is unsupported");
    const bool has_te = headers.contains("Transfer-Encoding");
    const bool has_cl = headers.contains("Content-Length");
    if (has_te && has_cl) {
        return invalid(operation, "Cannot generate both Transfer-Encoding and Content-Length");
    }
    auto length = content_length(headers, operation, has_te);
    if (!length) return length.error();
    if ((response.status_code < 200 || response.status_code == 204) && (has_cl || has_te)) {
        return invalid(operation, "Framing fields are forbidden for this status");
    }
    if (has_te) {
        if (response.version != HttpVersion::http_1_1() ||
            select_request_framing(headers).error != ParseError::none) {
            return invalid(operation, "Unsupported or invalid Transfer-Encoding");
        }
    }
    return serialize_head(response.version.to_string() + " " +
                          std::to_string(response.status_code) + " " + response.reason,
                          headers, limits, operation);
}

} // namespace

core::Result<std::string> serialize_response_head(const Response& response, SerializationLimits limits) {
    return response_head(response, response.headers, limits);
}

core::Result<std::string> serialize_response(const Response& response, SerializationLimits limits) {
    return serialize_response(response, Method::GET, limits);
}

core::Result<std::string> serialize_response(const Response& response, Method method, SerializationLimits limits) {
    return serialize_response(response, response.headers.contains("Transfer-Encoding") ?
        BodyFraming::chunked : BodyFraming::fixed_length, method, limits);
}

core::Result<std::string> serialize_response(
    const Response& response, BodyFraming framing, Method method, SerializationLimits limits
) {
    constexpr std::string_view operation = "serialize_response";
    const bool body_forbidden = response.status_code < 200 ||
                                response.status_code == 204 || response.status_code == 304;
    if (body_forbidden && !response.body.empty()) return invalid(operation, "Body is forbidden for this status");
    const bool no_body = body_forbidden || method == Method::HEAD;
    if (method == Method::CONNECT && response.status_code >= 200 && response.status_code < 300) {
        return invalid(operation, "CONNECT tunnels are unsupported");
    }
    if (!no_body && response.body.size() > limits.max_body_bytes) return invalid(operation, "Body exceeds output limit");
    if (!response.trailers.empty() && (no_body || framing != BodyFraming::chunked)) {
        return invalid(operation, "Trailers require a chunked body");
    }
    auto headers = response.headers;
    const bool has_te = headers.contains("Transfer-Encoding");
    const bool has_cl = headers.contains("Content-Length");
    if (has_te && has_cl) return invalid(operation, "Cannot generate both Transfer-Encoding and Content-Length");
    auto length = content_length(headers, operation, true);
    if (!length) return length.error();
    switch (framing) {
        case BodyFraming::none:
            if (!no_body) return invalid(operation, "A body-bearing response needs framing");
            break;
        case BodyFraming::fixed_length:
            if (has_te) return invalid(operation, "Transfer-Encoding conflicts with fixed-length output");
            if (!no_body && length.value() && *length.value() != response.body.size()) {
                return invalid(operation, "Content-Length does not match body size");
            }
            if (!body_forbidden && !has_cl) headers.add("Content-Length", std::to_string(response.body.size()));
            break;
        case BodyFraming::chunked:
            if (has_cl) return invalid(operation, "Content-Length conflicts with chunked output");
            if (!body_forbidden && !has_te) headers.add("Transfer-Encoding", "chunked");
            break;
        case BodyFraming::close_delimited:
            if (has_cl || has_te) return invalid(operation, "Framing fields conflict with close-delimited output");
            headers.set("Connection", "close");
            break;
        default:
            return invalid(operation, "Unknown output framing");
    }
    auto head = response_head(response, headers, limits);
    if (!head) return head.error();
    auto output = std::move(head.value());
    if (no_body) return output;
    if (framing == BodyFraming::chunked) {
        if (!response.body.empty()) {
            auto chunk = encode_chunk(response.body, limits);
            if (!chunk) return chunk.error();
            if (!append(output, chunk.value(), output.max_size())) return invalid(operation, "Response size overflow");
        }
        for (const auto& field : response.trailers) {
            if (is_prohibited_trailer(field.name, &headers)) return invalid(operation, "Prohibited trailer field");
        }
        auto final = encode_final_chunk(&response.trailers, limits);
        if (!final) return final.error();
        if (!append(output, final.value(), output.max_size())) return invalid(operation, "Response size overflow");
    } else if (!append(output, response.body, output.max_size())) {
        return invalid(operation, "Response size overflow");
    }
    return output;
}

core::Result<std::string> serialize_request_head(const Request& request, SerializationLimits limits) {
    constexpr std::string_view operation = "serialize_request_head";
    if (!request.version.is_valid()) return invalid(operation, "Unsupported HTTP version");
    const std::string_view method = request.raw_method.empty() ? to_string(request.method) : request.raw_method;
    if (method.size() > limits.max_head_bytes || !is_valid_header_name(method)) {
        return invalid(operation, "Invalid or oversized request method");
    }
    if (request.target.empty() || request.target.size() > limits.max_head_bytes) {
        return invalid(operation, "Empty or oversized request target");
    }
    for (unsigned char c : request.target) {
        if (c <= 0x20 || c >= 0x7f) return invalid(operation, "Invalid request target character");
    }
    auto length = content_length(request.headers, operation);
    if (!length) return length.error();
    return serialize_head(std::string(method) + " " + request.target + " " + request.version.to_string(),
                          request.headers, limits, operation);
}

core::Result<std::string> encode_chunk(std::span<const std::byte> chunk_data, SerializationLimits limits) {
    constexpr std::string_view operation = "encode_chunk";
    if (chunk_data.size() > limits.max_body_bytes) return invalid(operation, "Chunk exceeds body limit");
    std::array<char, 2 * sizeof(std::size_t)> digits{};
    const auto size = std::to_chars(digits.data(), digits.data() + digits.size(), chunk_data.size(), 16);
    const std::string_view size_text(digits.data(), static_cast<std::size_t>(size.ptr - digits.data()));
    if (size_text.size() + 4 > limits.max_head_bytes) return invalid(operation, "Chunk framing exceeds head limit");
    std::string result(size_text);
    result += "\r\n";
    if (!chunk_data.empty() && !append(result,
        std::string_view(reinterpret_cast<const char*>(chunk_data.data()), chunk_data.size()), result.max_size())) {
        return invalid(operation, "Chunk size overflow");
    }
    result += "\r\n";
    return result;
}

core::Result<std::string> encode_chunk(std::string_view chunk_data, SerializationLimits limits) {
    const auto* ptr = reinterpret_cast<const std::byte*>(chunk_data.data());
    return encode_chunk(std::span(ptr, chunk_data.size()), limits);
}

core::Result<std::string> encode_final_chunk(const HeaderMap* trailers, SerializationLimits limits) {
    constexpr std::string_view operation = "encode_final_chunk";
    std::string result;
    if (!append(result, "0\r\n", limits.max_head_bytes)) return invalid(operation, "Final chunk exceeds head limit");
    if (trailers) {
        for (const auto& field : *trailers) {
            if (!is_valid_header_name(field.name) || !is_valid_header_value(field.value) || is_prohibited_trailer(field.name)) {
                return invalid(operation, "Invalid or prohibited trailer field");
            }
            if (!append(result, field.name, limits.max_head_bytes) ||
                !append(result, ": ", limits.max_head_bytes) ||
                !append(result, field.value, limits.max_head_bytes) ||
                !append(result, "\r\n", limits.max_head_bytes)) return invalid(operation, "Trailers exceed head limit");
        }
    }
    if (!append(result, "\r\n", limits.max_head_bytes)) return invalid(operation, "Final chunk exceeds head limit");
    return result;
}

} // namespace http
