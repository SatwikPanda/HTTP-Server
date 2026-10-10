#pragma once

#include "core/clock.hpp"
#include "core/result.hpp"
#include "http/date.hpp"
#include "http/header.hpp"
#include "http/http_session.hpp"
#include "http/response.hpp"
#include "http/serializer.hpp"
#include "net/stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tests {

class Response {
public:
    Response() : impl_(std::make_unique<http::Response>()) {}
    explicit Response(http::Response resp) : impl_(std::make_unique<http::Response>(std::move(resp))) {}
    ~Response() = default;
    Response(Response&&) noexcept = default;
    Response& operator=(Response&&) noexcept = default;

    Response(const Response& other)
        : impl_(other.impl_ ? std::make_unique<http::Response>(*other.impl_) : nullptr) {}
    Response& operator=(const Response& other) {
        if (this != &other) {
            impl_ = other.impl_ ? std::make_unique<http::Response>(*other.impl_) : nullptr;
        }
        return *this;
    }

    [[nodiscard]] http::Response& raw() noexcept { return *impl_; }
    [[nodiscard]] const http::Response& raw() const noexcept { return *impl_; }

private:
    std::unique_ptr<http::Response> impl_;
};

inline const core::SystemTimePoint kFixedDemoWallTime =
    std::chrono::system_clock::from_time_t(784111777); // Sun, 06 Nov 1994 08:49:37 GMT

inline Response fixed_response() {
    http::Response resp(http::StatusCode::OK);
    resp.set_body("Hello, World!\r\n", "text/plain; charset=utf-8");
    resp.headers.set("Date", http::format_http_date(kFixedDemoWallTime));
    resp.headers.set("Connection", "close");
    return Response(std::move(resp));
}

inline core::Result<std::string> serialize(const Response& resp) {
    return http::serialize_response(resp.raw());
}

inline std::size_t content_length(const Response& resp) {
    return resp.raw().body.size();
}

inline std::string header(const Response& resp, std::string_view name) {
    auto val = resp.raw().headers.get(name);
    return val ? std::string(val.value()) : std::string{};
}

inline std::vector<std::string> headers(const Response& resp, std::string_view name) {
    std::vector<std::string> result;
    auto all = resp.raw().headers.get_all(name);
    result.reserve(all.size());
    for (auto sv : all) {
        result.emplace_back(sv);
    }
    return result;
}

inline void add_header(Response& resp, std::string_view name, std::string_view value) {
    resp.raw().headers.add(std::string(name), std::string(value));
}

class DemoStreamChannel final : public net::StreamChannel {
public:
    explicit DemoStreamChannel(std::string_view input, std::size_t max_write_chunk)
        : max_write_chunk_(max_write_chunk) {
        const auto* ptr = reinterpret_cast<const std::byte*>(input.data());
        input_data_.assign(ptr, ptr + input.size());
    }

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        if (closed_) {
            return net::ReadResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "read_some"));
        }
        if (read_offset_ >= input_data_.size()) {
            return net::ReadResult::eof();
        }
        std::size_t available = input_data_.size() - read_offset_;
        std::size_t to_copy = std::min(available, destination.size());
        std::memcpy(destination.data(), input_data_.data() + read_offset_, to_copy);
        read_offset_ += to_copy;
        return net::ReadResult::success(to_copy);
    }

    [[nodiscard]] net::WriteResult write_some(std::span<const std::byte> source) override {
        if (closed_) {
            return net::WriteResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "write_some"));
        }
        std::size_t to_write = source.size();
        if (max_write_chunk_ > 0 && to_write > max_write_chunk_) {
            to_write = max_write_chunk_;
        }
        const auto* src = reinterpret_cast<const char*>(source.data());
        output_data_.append(src, to_write);
        return net::WriteResult::success(to_write);
    }

    void close() noexcept override {
        closed_ = true;
    }

    [[nodiscard]] bool is_valid() const noexcept override {
        return !closed_;
    }

    [[nodiscard]] const std::string& output() const noexcept {
        return output_data_;
    }

private:
    std::vector<std::byte> input_data_;
    std::size_t read_offset_{0};
    std::size_t max_write_chunk_{0};
    std::string output_data_;
    bool closed_{false};
};

inline std::string fixed_http_roundtrip(std::string_view request, std::size_t max_write_chunk) {
    DemoStreamChannel channel(request, max_write_chunk);
    http::HttpSessionConfig config;
    config.response_body = "Hello, World!\r\n";
    config.content_type = "text/plain; charset=utf-8";
    config.fixed_wall_time = kFixedDemoWallTime;

    http::run_http_session(channel, config);
    return channel.output();
}

inline bool oversized_demo_header_is_rejected() {
    std::string huge_header(10000, 'X');
    DemoStreamChannel channel(huge_header, 0);
    http::HttpSessionConfig config;
    config.max_header_bytes = 4096;

    auto stats = http::run_http_session(channel, config);
    return stats.header_limit_exceeded;
}

} // namespace tests
