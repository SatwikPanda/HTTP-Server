#pragma once

#include "core/result.hpp"

#include <cstddef>

namespace http {

// Bounds applied while a request head is parsed. Every limit is checked as each
// byte arrives, so an oversized head is rejected without buffering the rest.
struct RequestLimits {
    // Request line excluding its CRLF.
    std::size_t max_request_line_bytes{8192};
    // Request target alone. Exceeding it maps to 414 URI Too Long.
    std::size_t max_target_bytes{8000};
    // Whole head: request line, field lines, and the terminating blank line.
    std::size_t max_header_bytes{8192};
    std::size_t max_header_count{100};
    // Decoded request body limit. Exceeding it maps to 413 Payload Too Large.
    std::size_t max_body_bytes{10 * 1024 * 1024};
    // Chunk extensions length bound.
    std::size_t max_chunk_extension_bytes{1024};
    // Trailer bytes and count bounds.
    std::size_t max_trailer_bytes{4096};
    std::size_t max_trailer_count{32};
};

// Rejects zero limits and anything above a 16 MiB hard ceiling.
[[nodiscard]] core::Result<void> validate_request_limits(const RequestLimits& limits);

} // namespace http
