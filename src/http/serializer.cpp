#include "http/serializer.hpp"

#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

namespace http {
namespace {

core::Error invalid(std::string_view operation, std::string message) {
    return core::make_error(core::ErrorCategory::invalid_argument,
                            std::string(operation), 0, std::move(message));
}

core::Result<std::optional<std::size_t>> content_length(
    const HeaderMap& headers, std::string_view operation
) {
    std::optional<std::size_t> length;
    for (const auto& field : headers) {
        if (!is_valid_header_name(field.name) || !is_valid_header_value(field.value)) {
            return invalid(operation, "Invalid header name or value");
        }
        if (header_name_equals(field.name, "Transfer-Encoding")) {
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
    SerializationLimits limits, std::string_view operation,
    std::optional<std::size_t> generated_length = std::nullopt
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
    if (generated_length &&
        (!append(output, "Content-Length: ", limits.max_head_bytes) ||
         !append(output, std::to_string(*generated_length), limits.max_head_bytes) ||
         !append(output, "\r\n", limits.max_head_bytes))) {
        return invalid(operation, "Headers exceed output head limit");
    }
    if (!append(output, "\r\n", limits.max_head_bytes)) {
        return invalid(operation, "Headers exceed output head limit");
    }
    return output;
}

core::Result<std::string> response_head(
    const Response& response, SerializationLimits limits,
    std::optional<std::size_t> generated_length = std::nullopt
) {
    constexpr std::string_view operation = "serialize_response_head";
    if (!response.version.is_valid()) return invalid(operation, "Unsupported HTTP version");
    if (!is_valid_status_code(response.status_code)) return invalid(operation, "Invalid HTTP status code");
    if (response.reason.size() > limits.max_head_bytes || !is_valid_reason_phrase(response.reason)) {
        return invalid(operation, "Invalid or oversized reason phrase");
    }
    auto length = content_length(response.headers, operation);
    if (!length) return length.error();
    if ((response.status_code < 200 || response.status_code == 204) && length.value()) {
        return invalid(operation, "Content-Length is forbidden for this status");
    }
    return serialize_head(response.version.to_string() + " " +
                          std::to_string(response.status_code) + " " + response.reason,
                          response.headers, limits, operation, generated_length);
}

} // namespace

core::Result<std::string> serialize_response_head(const Response& response, SerializationLimits limits) {
    return response_head(response, limits);
}

core::Result<std::string> serialize_response(const Response& response, SerializationLimits limits) {
    constexpr std::string_view operation = "serialize_response";
    if (response.body.size() > limits.max_body_bytes) return invalid(operation, "Body exceeds output limit");
    const bool body_forbidden = response.status_code < 200 ||
                                response.status_code == 204 || response.status_code == 304;
    if (body_forbidden && !response.body.empty()) return invalid(operation, "Body is forbidden for this status");
    auto length = content_length(response.headers, operation);
    if (!length) return length.error();
    if (!body_forbidden && length.value() && *length.value() != response.body.size()) {
        return invalid(operation, "Content-Length does not match body size");
    }
    auto head = response_head(response, limits,
        !body_forbidden && !length.value() ? std::optional(response.body.size()) : std::nullopt);
    if (!head) return head.error();
    auto output = std::move(head.value());
    if (response.body.size() > output.max_size() - output.size()) return invalid(operation, "Response size overflow");
    output += response.body;
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

} // namespace http
