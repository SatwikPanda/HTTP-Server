#pragma once

#include "core/result.hpp"
#include "http/framing.hpp"
#include "http/response.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace http {

// Owns at most one wire segment. Drain pending() with acknowledge() before
// enqueueing another segment. The window includes head/chunk/trailer overhead.
class ResponseWriter {
public:
    explicit ResponseWriter(std::size_t window = 8192) : window_(window) {}
    [[nodiscard]] core::Result<void> begin(Response head, Method method, BodyFraming framing,
                                          std::optional<std::uint64_t> length = {});
    [[nodiscard]] core::Result<void> buffered(const Response& response, Method method);
    [[nodiscard]] core::Result<void> body(std::span<const std::byte> bytes);
    [[nodiscard]] core::Result<void> finish(const HeaderMap& trailers = {});
    [[nodiscard]] core::Result<void> acknowledge(std::size_t bytes);
    void abort() noexcept;
    [[nodiscard]] std::span<const std::byte> pending() const noexcept;
    [[nodiscard]] std::size_t body_capacity() const noexcept;
    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] bool body_allowed() const noexcept { return body_allowed_; }
    [[nodiscard]] bool complete() const noexcept { return finished_ && pending().empty() && !failed_; }
    [[nodiscard]] std::uint64_t body_bytes() const noexcept { return body_bytes_; }
private:
    std::size_t window_;
    std::string pending_;
    std::size_t offset_{};
    std::optional<std::uint64_t> length_;
    std::uint64_t body_bytes_{};
    BodyFraming framing_{BodyFraming::none};
    bool started_{}, committed_{}, finished_{}, failed_{}, body_allowed_{};
};

} // namespace http
