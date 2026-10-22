#pragma once

#include "tree_fixture.hpp"

#include "concurrency/work.hpp"
#include "http/http_session.hpp"
#include "http/response_parser.hpp"
#include "server/static_service.hpp"

#include <cstring>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace server_test {

using Fields = std::initializer_list<std::pair<std::string_view, std::string_view>>;

inline std::string request(std::string_view target, std::string_view method = "GET", Fields fields = {}, bool close = false) {
    std::string text = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n";
    for (const auto& [name, value] : fields) text += std::string(name) + ": " + std::string(value) + "\r\n";
    if (close) text += "Connection: close\r\n";
    return text + "\r\n";
}

inline std::shared_ptr<const http::TransactionFactory> make_factory(const std::filesystem::path& root, server::StaticConfig config = {}) {
    config.document_root = root;
    return test::take(server::make_static_factory(config));
}

inline http::HttpSessionConfig session_config(std::shared_ptr<const http::TransactionFactory> factory,
                                              std::size_t max_output_bytes = 64 * 1024) {
    http::HttpSessionConfig config;
    config.application = std::move(factory);
    config.keep_alive = true;
    config.max_output_bytes = max_output_bytes;
    config.max_requests_per_connection = 1000;
    return config;
}

// A channel with controllable read/write granularity. The first write blocks
// once (would-block), and writes are capped at write_chunk.
class Channel final : public net::StreamChannel {
public:
    Channel(std::string input, std::size_t write_chunk = static_cast<std::size_t>(-1), std::size_t read_chunk = static_cast<std::size_t>(-1))
        : input_(std::move(input)), write_chunk_(write_chunk), read_chunk_(read_chunk) {}
    net::ReadResult read_some(std::span<std::byte> output) override {
        if (offset_ == input_.size()) return net::ReadResult::eof();
        const auto count = std::min({output.size(), read_chunk_, input_.size() - offset_});
        std::memcpy(output.data(), input_.data() + offset_, count);
        offset_ += count;
        return net::ReadResult::success(count);
    }
    net::WriteResult write_some(std::span<const std::byte> input) override {
        if (block_) { block_ = false; return net::WriteResult::would_block_result(); }
        const auto count = std::min(write_chunk_, input.size());
        output.append(reinterpret_cast<const char*>(input.data()), count);
        ++writes;
        return net::WriteResult::success(count);
    }
    bool is_valid() const noexcept override { return valid_; }
    void close() noexcept override { valid_ = false; }
    std::string output;
    std::size_t writes{};
private:
    std::string input_;
    std::size_t offset_{}, write_chunk_, read_chunk_;
    bool block_{true}, valid_{true};
};

struct Parsed {
    int status{};
    http::HeaderMap headers;
    std::string body;
    std::size_t end{}; // offset just past this response in the byte stream
};

// Parses consecutive responses for the given request methods. Anything left
// over (or a parse failure) is reported through `complete`.
struct ParsedStream {
    std::vector<Parsed> responses;
    bool complete{};      // every expected response parsed, nothing left over
    std::size_t consumed{};
};

inline ParsedStream parse_stream(const std::string& bytes, const std::vector<http::Method>& methods) {
    ParsedStream stream;
    http::RequestLimits limits;
    limits.max_body_bytes = 64 * 1024 * 1024;
    for (const auto method : methods) {
        http::ResponseParser parser(method, limits);
        const auto result = parser.consume(std::string_view(bytes).substr(stream.consumed));
        if (result.event != http::ParseEvent::complete) return stream;
        stream.consumed += result.consumed;
        stream.responses.push_back({parser.response().status_code, parser.response().headers, parser.response().body, stream.consumed});
    }
    stream.complete = stream.consumed == bytes.size();
    return stream;
}

inline std::string header(const Parsed& response, std::string_view name) {
    const auto value = response.headers.get(name);
    return value ? std::string(*value) : std::string{};
}

// Drives an HttpSession one action at a time with the real file executor, so
// tests can interleave faults between jobs and observe each submitted job.
struct JobRecord {
    concurrency::WorkRequest request;
    concurrency::WorkEvent result;
    std::size_t output_bytes_before{}; // bytes written downstream when the job started
};

class ManualDriver {
public:
    ManualDriver(http::HttpSession& session, std::string input, std::size_t write_chunk = static_cast<std::size_t>(-1))
        : session_(session), input_(std::move(input)), write_chunk_(write_chunk) {}

    // Called before a job executes (may mutate the tree) and after (may rewrite the result).
    std::function<void(const concurrency::WorkAction&)> before_job;
    std::function<void(const concurrency::WorkAction&, concurrency::WorkEvent&)> after_job;
    // Called with the number of response bytes written so far after each write.
    std::function<void(std::size_t)> after_write;
    // Stop feeding events (simulating a lost client) once this many bytes were written.
    std::optional<std::size_t> lose_client_after;

    // Returns true when the session closed; false if it stalled or hit the step cap.
    bool run() {
        auto action = session_.initial_action();
        for (unsigned step = 0; step < 2'000'000; ++step) {
            if (action.is_close()) { closed_error = action.as_close()->error; return true; }
            if (const auto* read = action.as_read()) {
                const auto count = std::min(read->max_bytes, input_.size() - input_offset_);
                if (count == 0) { action = session_.on_event(concurrency::ReadEofEvent{read->id}); continue; }
                const auto data = std::as_bytes(std::span(input_.data() + input_offset_, count));
                input_offset_ += count;
                action = session_.on_event(concurrency::ReadDataEvent{data, read->id});
            } else if (const auto* write = action.as_write()) {
                if (lose_client_after && output.size() >= *lose_client_after) {
                    action = session_.on_event(concurrency::ErrorEvent{core::make_error(core::ErrorCategory::connection_reset, "client lost"), write->id});
                    continue;
                }
                const auto count = std::min(write_chunk_, write->data.size());
                output.append(reinterpret_cast<const char*>(write->data.data()), count);
                max_write_request = std::max(max_write_request, write->data.size());
                if (after_write) after_write(output.size());
                action = session_.on_event(concurrency::WriteCompleteEvent{count, write->id});
            } else if (const auto* wait = action.as_wait()) {
                action = session_.on_event(concurrency::ReadyEvent{true, true, wait->id});
            } else if (const auto* work = action.as_work()) {
                if (before_job) before_job(*work);
                auto result = executor_.execute(*work);
                if (after_job) after_job(*work, result);
                jobs.push_back({work->request, result, output.size()});
                action = session_.on_event(result);
            } else return false; // no action: the session stalled
        }
        return false;
    }

    std::string output;
    std::vector<JobRecord> jobs;
    std::size_t max_write_request{};
    core::Error closed_error;

private:
    http::HttpSession& session_;
    std::string input_;
    std::size_t input_offset_{}, write_chunk_;
    concurrency::ReferenceWorkExecutor executor_;
};

// Open descriptors of this process, where the platform lets us count them.
inline std::optional<std::size_t> open_descriptor_count() {
#if defined(__linux__)
    const char* directory = "/proc/self/fd";
#elif defined(__APPLE__)
    const char* directory = "/dev/fd";
#else
    return std::nullopt;
#endif
    std::error_code error;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) { (void)entry; ++count; }
    if (error) return std::nullopt;
    return count;
}

inline std::string pattern_bytes(std::size_t size, unsigned seed = 1) {
    std::string bytes(size, '\0');
    unsigned state = seed * 2654435761u + 12345u;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<char>(state >> 24);
    }
    return bytes;
}

inline void unsupported(const char* what) { std::cout << "UNSUPPORTED (not counted as exercised): " << what << '\n'; }
} // namespace server_test

namespace server_test {
// One request through the real session, factory and file executor.
inline Parsed fetch_response(const std::shared_ptr<const http::TransactionFactory>& factory, std::string_view target,
                             std::string_view method = "GET", Fields fields = {}, std::size_t window = 64 * 1024) {
    Channel channel(request(target, method, fields, true));
    auto stats = http::run_http_session(channel, session_config(factory, window));
    CHECK(stats.completed_cleanly);
    auto parsed = parse_stream(channel.output, {method == "HEAD" ? http::Method::HEAD : http::Method::GET});
    CHECK(parsed.complete && parsed.responses.size() == 1);
    return parsed.responses[0];
}
} // namespace server_test
