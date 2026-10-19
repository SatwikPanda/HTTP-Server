#include "http/response_writer.hpp"

#include "http/serializer.hpp"

#include <algorithm>
#include <limits>

namespace http {
namespace {
core::Error invalid() { return core::make_error(core::ErrorCategory::invalid_argument, "HTTP response writer"); }
}

core::Result<void> ResponseWriter::buffered(const Response& response, Method method) {
    if (started_ || failed_) return invalid();
    auto wire = serialize_response(response, method, {window_, window_});
    if (!wire) return wire.error();
    if (wire.value().size() > window_) return invalid();
    pending_ = std::move(wire.value());
    body_bytes_ = method == Method::HEAD ? 0 : response.body.size();
    started_ = true; finished_ = true;
    return {};
}

core::Result<void> ResponseWriter::begin(Response head, Method method, BodyFraming framing,
                                        std::optional<std::uint64_t> length) {
    if (started_ || failed_ || !head.body.empty() || !head.trailers.empty() || head.status_code < 200) return invalid();
    const auto existing = select_response_framing(head, method);
    if (existing.error != ParseError::none) return invalid();
    if (head.headers.contains("Transfer-Encoding") && framing != BodyFraming::chunked) return invalid();
    const auto fields = select_request_framing(head.headers);
    if (head.headers.contains("Content-Length") && (!length || fields.content_length != *length)) return invalid();
    body_allowed_ = method != Method::HEAD && head.status_code != 204 && head.status_code != 304;
    if (head.status_code == 204) {
        if (length || head.headers.contains("Content-Length") || head.headers.contains("Transfer-Encoding")) return invalid();
        framing = BodyFraming::none;
    } else if (framing == BodyFraming::fixed_length) {
        if (!length || head.headers.contains("Transfer-Encoding")) return invalid();
        head.headers.set("Content-Length", std::to_string(*length));
    } else if (framing == BodyFraming::chunked) {
        if (length || head.headers.contains("Content-Length") || head.version == HttpVersion::http_1_0()) return invalid();
        head.headers.set("Transfer-Encoding", "chunked");
    } else if (framing == BodyFraming::close_delimited) {
        if (length || head.headers.contains("Content-Length") || head.headers.contains("Transfer-Encoding")) return invalid();
        head.headers.set("Connection", "close");
    } else if (body_allowed_) return invalid();
    auto wire = serialize_response_head(head, {window_, window_});
    if (!wire) return wire.error();
    pending_ = std::move(wire.value());
    length_ = length;
    framing_ = framing;
    started_ = true;
    return {};
}

std::span<const std::byte> ResponseWriter::pending() const noexcept {
    return std::as_bytes(std::span(pending_.data() + offset_, pending_.size() - offset_));
}

std::size_t ResponseWriter::body_capacity() const noexcept {
    if (!started_ || finished_ || failed_ || !body_allowed_ || !pending().empty()) return 0;
    // 16 hex digits plus CRLF before/after the body on a 64-bit platform.
    return framing_ == BodyFraming::chunked ? window_ - std::min(window_, std::size_t{20}) : window_;
}

core::Result<void> ResponseWriter::body(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > body_capacity() || bytes.size() > UINT64_MAX - body_bytes_ ||
        (length_ && (body_bytes_ > *length_ || bytes.size() > *length_ - body_bytes_))) return invalid();
    if (framing_ == BodyFraming::chunked) {
        auto encoded = encode_chunk(bytes, {window_, window_});
        if (!encoded) return encoded.error();
        pending_ = std::move(encoded.value());
    } else pending_.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    offset_ = 0;
    body_bytes_ += bytes.size();
    return {};
}

core::Result<void> ResponseWriter::finish(const HeaderMap& trailers) {
    if (!started_ || finished_ || failed_ || !pending().empty() ||
        (body_allowed_ && length_ && body_bytes_ != *length_) ||
        (!trailers.empty() && (!body_allowed_ || framing_ != BodyFraming::chunked))) return invalid();
    if (body_allowed_ && framing_ == BodyFraming::chunked) {
        auto encoded = encode_final_chunk(&trailers, {window_, window_});
        if (!encoded) return encoded.error();
        pending_ = std::move(encoded.value());
        offset_ = 0;
    }
    finished_ = true;
    return {};
}

core::Result<void> ResponseWriter::acknowledge(std::size_t bytes) {
    if (failed_ || bytes == 0 || bytes > pending().size()) return invalid();
    committed_ = true;
    offset_ += bytes;
    if (offset_ == pending_.size()) { pending_.clear(); offset_ = 0; }
    return {};
}

void ResponseWriter::abort() noexcept {
    failed_ = true;
    pending_.clear();
    offset_ = 0;
}
} // namespace http
