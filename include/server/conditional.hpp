#pragma once

#include "core/clock.hpp"
#include "http/header.hpp"

#include <cstdint>
#include <string>

namespace server {

// The selected representation of a static file: opened-stream length plus the
// path's modification time. Under the controlled-tree assumption (see the
// static handoff) these identify the bytes a GET would send.
struct Representation {
    std::uint64_t size{};
    core::SystemTimePoint modified{};
};

// Weak entity tag W/"<size>-<mtime>", both as lower-case hex. The time is the
// platform's native system_clock tick count (not rounded to seconds), reinterpreted
// as unsigned so pre-epoch times still format. A size/mtime pair is not a
// strong content hash, which is why the tag is always weak.
[[nodiscard]] std::string make_etag(const Representation& representation);

// IMF-fixdate at whole-second resolution, or empty when the year cannot be
// formatted (the caller then omits Last-Modified).
[[nodiscard]] std::string make_last_modified(const Representation& representation);

enum class Conditional { proceed, not_modified };

// GET/HEAD precondition evaluation (RFC 9110 section 13.2.2, steps for
// If-None-Match and If-Modified-Since). If-None-Match, when present, decides
// alone and If-Modified-Since is not consulted, even when the tag does not match
// or the field is malformed. A malformed or unusable condition never produces
// 304: the full response is sent. If-Modified-Since is used only when
// If-None-Match is absent, must appear once, and must parse as an HTTP date.
[[nodiscard]] Conditional evaluate_conditions(const http::HeaderMap& request_headers,
                                              const Representation& representation,
                                              core::SystemTimePoint now);

} // namespace server
