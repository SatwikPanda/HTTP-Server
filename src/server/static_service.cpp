#include "server/static_service.hpp"

#include "server/conditional.hpp"
#include "server/mime.hpp"
#include "server/path_policy.hpp"

#include <algorithm>
#include <variant>

namespace server {
namespace {
using concurrency::FileCloseWork;
using concurrency::FileFailure;
using concurrency::FileOpenWork;
using concurrency::FileReadWork;
using concurrency::WorkEvent;
using concurrency::WorkStatus;

struct Shared {
    PathPolicy policy;
    StaticConfig config;
};

core::Error work_error(const WorkEvent& event, const char* operation) {
    if (event.error) return event.error;
    return core::make_error(core::ErrorCategory::io_error, operation);
}

// Bounded error responses never contain local paths or native diagnostics.
http::BufferedResponse error_response(http::StatusCode status) {
    http::Response response(status);
    response.set_body(std::to_string(response.status_code) + " " + response.reason + "\r\n");
    if (status == http::StatusCode::MethodNotAllowed) response.headers.set("Allow", "GET, HEAD");
    return {std::move(response)};
}

class StaticTransaction final : public http::Transaction {
public:
    StaticTransaction(std::shared_ptr<const Shared> shared, const http::Request& request,
                      const http::TransactionContext& context)
        : shared_(std::move(shared)), request_(request), steady_(context.steady_clock), wall_(context.wall_clock) {}

    http::TransactionStep on_event(const http::TransactionEvent& event) override {
        if (std::holds_alternative<http::StartTransaction>(event)) return start();
        if (const auto* drained = std::get_if<http::OutputDrained>(&event)) return output_drained(drained->body_capacity);
        return work_completed(std::get<WorkEvent>(event));
    }

private:
    // opening: waiting for the open job. closing_before_head: HEAD/304 holds a
    // prepared head until the handle is released. streaming: head sent, reading
    // on demand. closing_after_body: all bytes sent, releasing the handle.
    enum class State { start, opening, closing_before_head, streaming, closing_after_body, done };

    http::TransactionStep start() {
        if (state_ != State::start) return failure(core::make_error(core::ErrorCategory::invalid_argument, "static transaction state"));
        if (request_.method != http::Method::GET && request_.method != http::Method::HEAD) {
            return error_response(http::StatusCode::MethodNotAllowed);
        }
        const auto resolved = shared_->policy.resolve(request_.target);
        if (resolved.verdict == TargetVerdict::invalid) return error_response(http::StatusCode::BadRequest);
        if (resolved.verdict == TargetVerdict::forbidden) return error_response(http::StatusCode::Forbidden);
        trailing_slash_ = resolved.trailing_slash;
        content_type_ = std::string(mime_type_for(resolved.segments.back()));
        state_ = State::opening;
        return work(FileOpenWork{resolved.path});
    }

    http::TransactionStep work_completed(const WorkEvent& result) {
        switch (state_) {
        case State::opening: return opened(result);
        case State::closing_before_head:
            // The handle is no longer needed; a failed close cannot change the answer.
            if (interrupted(result.status)) return failure(work_error(result, "static file close"));
            state_ = State::streaming;
            return std::move(prepared_head_);
        case State::streaming: return read_completed(result);
        case State::closing_after_body:
            if (interrupted(result.status)) return failure(work_error(result, "static file close"));
            state_ = State::done;
            return http::ResponseFinished{};
        default: return failure(core::make_error(core::ErrorCategory::invalid_argument, "static transaction state"));
        }
    }

    http::TransactionStep opened(const WorkEvent& result) {
        if (result.status != WorkStatus::complete) return open_failed(result);
        file_ = result.file;
        const auto& info = result.info;
        // The open job already refuses links and non-regular files; these checks
        // keep the policy true if the executor changes, and compare containment
        // by path components rather than text prefix.
        if (!file_ || info.has_link_component || info.directory || !info.regular ||
            !shared_->policy.contains(info.canonical_path)) return error_response(http::StatusCode::Forbidden);
        if (trailing_slash_) return error_response(http::StatusCode::NotFound); // a file named as a directory
        length_ = info.size; // the opened stream's length is authoritative
        const Representation representation{info.size, info.modified};
        const bool head_only = request_.method == http::Method::HEAD;

        http::Response head(http::StatusCode::OK);
        head.headers.set("ETag", make_etag(representation));
        if (auto modified = make_last_modified(representation); !modified.empty()) head.headers.set("Last-Modified", std::move(modified));
        http::ResponseHead step{{}, http::BodyFraming::fixed_length, length_};
        if (evaluate_conditions(request_.headers, representation, wall_.now()) == Conditional::not_modified) {
            // 304 repeats validator metadata only; it invents no length or type.
            head.set_status(http::StatusCode::NotModified);
            step.framing = http::BodyFraming::none;
            step.length.reset();
        } else {
            head.headers.set("Content-Type", content_type_);
        }
        step.response = std::move(head);
        if (!head_only && step.response.status_code == 200) {
            state_ = State::streaming;
            return step;
        }
        // HEAD and 304 schedule no reads; release the handle, then send the head.
        prepared_head_ = std::move(step);
        state_ = State::closing_before_head;
        return work(FileCloseWork{file_});
    }

    http::TransactionStep open_failed(const WorkEvent& result) {
        if (result.status == WorkStatus::failed) {
            switch (result.file_failure) {
            case FileFailure::missing: return error_response(http::StatusCode::NotFound);
            case FileFailure::forbidden:
            case FileFailure::not_regular: return error_response(http::StatusCode::Forbidden);
            default: break;
            }
        }
        const auto status = result.status == WorkStatus::timed_out ? http::StatusCode::ServiceUnavailable
                                                                    : http::StatusCode::InternalServerError;
        return http::TransactionFailure{work_error(result, "static file open"), status};
    }

    http::TransactionStep output_drained(std::size_t body_capacity) {
        if (state_ != State::streaming || !file_) return failure(core::make_error(core::ErrorCategory::invalid_argument, "static transaction state"));
        if (bytes_read_ == length_) {
            state_ = State::closing_after_body;
            return work(FileCloseWork{file_});
        }
        if (body_capacity == 0) return failure(core::make_error(core::ErrorCategory::invalid_argument, "static output window"));
        // Never read past the advertised length, even if the file has grown, and
        // never more than the writer can take right now.
        requested_ = static_cast<std::size_t>(std::min<std::uint64_t>(
            {body_capacity, length_ - bytes_read_, shared_->config.max_chunk_bytes}));
        return work(FileReadWork{file_, bytes_read_, requested_});
    }

    http::TransactionStep read_completed(const WorkEvent& result) {
        if (result.status == WorkStatus::eof) {
            // EOF before the advertised length is a truncated file, never success.
            return failure(core::make_error(core::ErrorCategory::eof, "static file read", 0, "file shorter than advertised length"));
        }
        if (result.status != WorkStatus::progress && result.status != WorkStatus::complete) return failure(work_error(result, "static file read"));
        if (!result.bytes || result.bytes->empty() || result.bytes->size() > requested_ ||
            result.bytes->size() > length_ - bytes_read_) {
            return failure(core::make_error(core::ErrorCategory::io_error, "static file read", 0, "invalid read result"));
        }
        bytes_read_ += result.bytes->size();
        return http::ResponseBody{result.bytes};
    }

    [[nodiscard]] static bool interrupted(WorkStatus status) noexcept {
        return status == WorkStatus::cancelled || status == WorkStatus::timed_out || status == WorkStatus::rejected;
    }
    [[nodiscard]] http::TransactionFailure failure(core::Error error) const {
        return {std::move(error), http::StatusCode::InternalServerError};
    }
    [[nodiscard]] http::TransactionWork work(concurrency::WorkRequest request) const {
        return {std::move(request), steady_.now() + shared_->config.work_timeout};
    }

    std::shared_ptr<const Shared> shared_;
    const http::Request& request_;
    const core::SteadyClock& steady_;
    const core::SystemClock& wall_;
    State state_{State::start};
    concurrency::FileToken file_;
    std::string content_type_;
    http::ResponseHead prepared_head_;
    std::uint64_t length_{}, bytes_read_{};
    std::size_t requested_{};
    bool trailing_slash_{};
};

class StaticFactory final : public http::TransactionFactory {
public:
    explicit StaticFactory(std::shared_ptr<const Shared> shared) : shared_(std::move(shared)) {}
    core::Result<std::unique_ptr<http::Transaction>> create(const http::Request& request,
                                                            const http::TransactionContext& context) const override {
        return std::unique_ptr<http::Transaction>(std::make_unique<StaticTransaction>(shared_, request, context));
    }

private:
    std::shared_ptr<const Shared> shared_;
};
} // namespace

core::Result<std::shared_ptr<const http::TransactionFactory>> make_static_factory(const StaticConfig& config) {
    constexpr std::size_t max_chunk = 16 * 1024 * 1024; // the file port's validated ceiling
    if (config.max_chunk_bytes == 0 || config.max_chunk_bytes > max_chunk) {
        return core::make_error(core::ErrorCategory::invalid_argument, "static config", 0, "max_chunk_bytes out of range");
    }
    if (config.work_timeout <= std::chrono::milliseconds::zero()) {
        return core::make_error(core::ErrorCategory::invalid_argument, "static config", 0, "work_timeout must be positive");
    }
    auto policy = PathPolicy::create(config.document_root, config.index_file);
    if (!policy) return policy.error();
    auto shared = std::make_shared<const Shared>(Shared{std::move(policy.value()), config});
    return std::shared_ptr<const http::TransactionFactory>(std::make_shared<StaticFactory>(std::move(shared)));
}
} // namespace server
