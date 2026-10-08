#pragma once

#include "core/error.hpp"

#include <cstddef>
#include <utility>

namespace net {

enum class ReadStatus {
    ok,
    eof,
    would_block,
    error
};

struct ReadResult {
    ReadStatus status{ReadStatus::ok};
    std::size_t bytes_transferred{0};
    core::Error error{};

    [[nodiscard]] bool is_ok() const noexcept {
        return status == ReadStatus::ok;
    }

    [[nodiscard]] bool is_eof() const noexcept {
        return status == ReadStatus::eof;
    }

    [[nodiscard]] bool would_block() const noexcept {
        return status == ReadStatus::would_block;
    }

    [[nodiscard]] bool is_error() const noexcept {
        return status == ReadStatus::error;
    }

    static ReadResult success(std::size_t bytes) noexcept {
        return ReadResult{ReadStatus::ok, bytes, {}};
    }

    static ReadResult eof() noexcept {
        return ReadResult{ReadStatus::eof, 0, {}};
    }

    static ReadResult would_block_result() noexcept {
        return ReadResult{ReadStatus::would_block, 0, {}};
    }

    static ReadResult failure(core::Error err) noexcept {
        return ReadResult{ReadStatus::error, 0, std::move(err)};
    }
};

enum class WriteStatus {
    ok,
    would_block,
    error
};

struct WriteResult {
    WriteStatus status{WriteStatus::ok};
    std::size_t bytes_transferred{0};
    core::Error error{};

    [[nodiscard]] bool is_ok() const noexcept {
        return status == WriteStatus::ok;
    }

    [[nodiscard]] bool would_block() const noexcept {
        return status == WriteStatus::would_block;
    }

    [[nodiscard]] bool is_error() const noexcept {
        return status == WriteStatus::error;
    }

    static WriteResult success(std::size_t bytes) noexcept {
        return WriteResult{WriteStatus::ok, bytes, {}};
    }

    static WriteResult would_block_result() noexcept {
        return WriteResult{WriteStatus::would_block, 0, {}};
    }

    static WriteResult failure(core::Error err) noexcept {
        return WriteResult{WriteStatus::error, 0, std::move(err)};
    }
};

} // namespace net
