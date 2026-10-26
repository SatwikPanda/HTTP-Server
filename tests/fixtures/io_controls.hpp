#pragma once

#include "concurrency/session.hpp"
#include "concurrency/work.hpp"
#include "core/clock.hpp"
#include "core/error.hpp"
#include "net/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace test {

// Programmable StreamChannel that injects deterministic short reads/writes,
// would-block outcomes, premature EOF, and specific transport errors.
class ControlledByteChannel final : public net::StreamChannel {
public:
    explicit ControlledByteChannel(
        std::string input,
        std::size_t max_read_chunk = 65536,
        std::size_t max_write_chunk = 65536
    )
        : input_(std::move(input)),
          max_read_chunk_(std::max<std::size_t>(1, max_read_chunk)),
          max_write_chunk_(std::max<std::size_t>(1, max_write_chunk)) {}

    void set_read_would_block_every(std::size_t n) noexcept { read_would_block_every_ = n; }
    void set_write_would_block_every(std::size_t n) noexcept { write_would_block_every_ = n; }
    void fail_read_after_bytes(std::size_t bytes, core::Error error) {
        fail_read_after_ = bytes;
        read_failure_ = std::move(error);
    }
    void fail_write_after_bytes(std::size_t bytes, core::Error error) {
        fail_write_after_ = bytes;
        write_failure_ = std::move(error);
    }

    net::ReadResult read_some(std::span<std::byte> buffer) override {
        if (!valid_) {
            return net::ReadResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "read"));
        }
        ++read_calls_;
        if (fail_read_after_ && offset_ >= *fail_read_after_) {
            return net::ReadResult::failure(read_failure_);
        }
        if (read_would_block_every_ > 0 && (read_calls_ % read_would_block_every_) == 0) {
            return net::ReadResult::would_block_result();
        }
        if (offset_ >= input_.size()) {
            return net::ReadResult::eof();
        }
        std::size_t count = std::min({buffer.size(), max_read_chunk_, input_.size() - offset_});
        if (fail_read_after_ && offset_ + count > *fail_read_after_) {
            count = *fail_read_after_ - offset_;
        }
        std::memcpy(buffer.data(), input_.data() + offset_, count);
        offset_ += count;
        return net::ReadResult::success(count);
    }

    net::WriteResult write_some(std::span<const std::byte> data) override {
        if (!valid_) {
            return net::WriteResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "write"));
        }
        ++write_calls_;
        if (fail_write_after_ && output_.size() >= *fail_write_after_) {
            return net::WriteResult::failure(write_failure_);
        }
        if (write_would_block_every_ > 0 && (write_calls_ % write_would_block_every_) == 0) {
            return net::WriteResult::would_block_result();
        }
        std::size_t count = std::min(max_write_chunk_, data.size());
        if (fail_write_after_ && output_.size() + count > *fail_write_after_) {
            count = *fail_write_after_ - output_.size();
        }
        output_.append(reinterpret_cast<const char*>(data.data()), count);
        return net::WriteResult::success(count);
    }

    [[nodiscard]] bool is_valid() const noexcept override { return valid_; }
    void close() noexcept override { valid_ = false; }

    [[nodiscard]] const std::string& output() const noexcept { return output_; }
    [[nodiscard]] std::size_t bytes_read() const noexcept { return offset_; }
    [[nodiscard]] std::size_t read_calls() const noexcept { return read_calls_; }
    [[nodiscard]] std::size_t write_calls() const noexcept { return write_calls_; }

private:
    std::string input_;
    std::string output_;
    std::size_t offset_{0};
    std::size_t max_read_chunk_;
    std::size_t max_write_chunk_;
    std::size_t read_calls_{0};
    std::size_t write_calls_{0};
    std::size_t read_would_block_every_{0};
    std::size_t write_would_block_every_{0};
    std::optional<std::size_t> fail_read_after_;
    std::optional<std::size_t> fail_write_after_;
    core::Error read_failure_{core::make_error(core::ErrorCategory::connection_reset, "read")};
    core::Error write_failure_{core::make_error(core::ErrorCategory::connection_reset, "write")};
    bool valid_{true};
};

// Thread-safe wrappers around ManualSteadyClock and ManualSystemClock so concurrent
// driver/helper tests never mutate manual clocks without synchronization.
class SynchronizedManualSteadyClock final : public core::SteadyClock {
public:
    explicit SynchronizedManualSteadyClock(core::SteadyTimePoint start = core::SteadyTimePoint{}) noexcept
        : clock_(start) {}

    [[nodiscard]] core::SteadyTimePoint now() const noexcept override {
        std::lock_guard lock(mutex_);
        return clock_.now();
    }

    void advance(std::chrono::nanoseconds duration) noexcept {
        std::lock_guard lock(mutex_);
        clock_.advance(duration);
    }

    void set(core::SteadyTimePoint tp) noexcept {
        std::lock_guard lock(mutex_);
        clock_.set(tp);
    }

private:
    mutable std::mutex mutex_;
    core::ManualSteadyClock clock_;
};

class SynchronizedManualSystemClock final : public core::SystemClock {
public:
    explicit SynchronizedManualSystemClock(core::SystemTimePoint start = core::SystemTimePoint{}) noexcept
        : clock_(start) {}

    [[nodiscard]] core::SystemTimePoint now() const noexcept override {
        std::lock_guard lock(mutex_);
        return clock_.now();
    }

    void advance(std::chrono::nanoseconds duration) noexcept {
        std::lock_guard lock(mutex_);
        clock_.advance(duration);
    }

    void set(core::SystemTimePoint tp) noexcept {
        std::lock_guard lock(mutex_);
        clock_.set(tp);
    }

private:
    mutable std::mutex mutex_;
    core::ManualSystemClock clock_;
};

// FNV-1a 64-bit hash and independent wire oracle that inspects HTTP responses
// without relying exclusively on the production ResponseParser under test.
[[nodiscard]] inline std::uint64_t fnv1a_64(std::string_view bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : bytes) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct IndependentWireFrame {
    int status_code{0};
    std::string status_line{};
    std::string raw_header_block{};
    std::string body{};
    std::size_t total_wire_bytes{0};
    bool valid{false};

    [[nodiscard]] bool has_header_line(std::string_view exact_line) const noexcept {
        const std::string needle = std::string(exact_line) + "\r\n";
        return raw_header_block.find(needle) != std::string::npos;
    }
};

// Independent wire-format oracle for fixed-length and bodyless HTTP/1.x responses.
[[nodiscard]] inline IndependentWireFrame inspect_fixed_wire_response(
    std::string_view wire,
    bool bodyless_method = false
) {
    IndependentWireFrame out{};
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return out;
    }
    const auto first_crlf = wire.find("\r\n");
    if (first_crlf == std::string_view::npos || first_crlf > header_end) {
        return out;
    }
    out.status_line = std::string(wire.substr(0, first_crlf));
    out.raw_header_block = std::string(wire.substr(first_crlf + 2, header_end - first_crlf));

    // Expect "HTTP/1.x NNN ..."
    if (out.status_line.size() < 12 || !out.status_line.starts_with("HTTP/1.")) {
        return out;
    }
    int code = 0;
    for (std::size_t i = 9; i < 12; ++i) {
        const char ch = out.status_line[i];
        if (ch < '0' || ch > '9') return out;
        code = code * 10 + (ch - '0');
    }
    out.status_code = code;

    const std::size_t body_offset = header_end + 4;
    if (bodyless_method || (code >= 100 && code < 200) || code == 204 || code == 304) {
        out.total_wire_bytes = body_offset;
        out.valid = true;
        return out;
    }

    // Extract Content-Length or Transfer-Encoding: chunked if present
    std::optional<std::size_t> content_len;
    bool is_chunked = false;
    std::string_view rem = out.raw_header_block;
    while (!rem.empty()) {
        const auto eol = rem.find("\r\n");
        const auto line = rem.substr(0, eol);
        if (eol == std::string_view::npos) rem = {};
        else rem.remove_prefix(eol + 2);

        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto name = line.substr(0, colon);
            auto val = line.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
            if (name == "Content-Length" || name == "content-length") {
                std::size_t parsed = 0;
                for (char ch : val) {
                    if (ch < '0' || ch > '9') return out;
                    parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
                }
                content_len = parsed;
            } else if ((name == "Transfer-Encoding" || name == "transfer-encoding") && val == "chunked") {
                is_chunked = true;
            }
        }
    }

    if (is_chunked) {
        std::size_t pos = body_offset;
        while (pos < wire.size()) {
            const auto line_end = wire.find("\r\n", pos);
            if (line_end == std::string_view::npos) return out;
            auto size_str = wire.substr(pos, line_end - pos);
            const auto semi = size_str.find(';');
            if (semi != std::string_view::npos) size_str = size_str.substr(0, semi);
            if (size_str.empty()) return out;
            std::size_t chunk_len = 0;
            for (char ch : size_str) {
                chunk_len *= 16;
                if (ch >= '0' && ch <= '9') chunk_len += static_cast<std::size_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') chunk_len += static_cast<std::size_t>(10 + ch - 'a');
                else if (ch >= 'A' && ch <= 'F') chunk_len += static_cast<std::size_t>(10 + ch - 'A');
                else return out;
            }
            pos = line_end + 2;
            if (chunk_len == 0) {
                const auto trailer_end = wire.find("\r\n", pos);
                if (trailer_end == std::string_view::npos) return out;
                out.total_wire_bytes = trailer_end + 2;
                out.valid = true;
                return out;
            }
            if (wire.size() < pos + chunk_len + 2) return out;
            out.body.append(wire.substr(pos, chunk_len));
            if (wire.substr(pos + chunk_len, 2) != "\r\n") return out;
            pos += chunk_len + 2;
        }
        return out;
    }

    if (!content_len) {
        out.body = std::string(wire.substr(body_offset));
        out.total_wire_bytes = wire.size();
        out.valid = true;
        return out;
    }

    if (wire.size() < body_offset + *content_len) {
        return out;
    }
    out.body = std::string(wire.substr(body_offset, *content_len));
    out.total_wire_bytes = body_offset + *content_len;
    out.valid = true;
    return out;
}

} // namespace test
