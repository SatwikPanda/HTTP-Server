#include "http/reference_application.hpp"

#include "http/date.hpp"
#include "http/response_parser.hpp"
#include "http/serializer.hpp"

#include <algorithm>

namespace http {
namespace {
using namespace concurrency;
TransactionFailure failure(core::Error error = {}) {
    if (!error) error = core::make_error(core::ErrorCategory::io_error, "reference transaction");
    return {std::move(error), StatusCode::BadGateway};
}
SharedBytes bytes(std::string_view text) {
    auto data = std::as_bytes(std::span(text.data(), text.size()));
    return std::make_shared<const Bytes>(data.begin(), data.end());
}
class FileTransaction final : public Transaction {
public:
    FileTransaction(std::filesystem::path path, const TransactionContext& context)
        : path_(std::move(path)), clock_(context.steady_clock) {}
    TransactionStep on_event(const TransactionEvent& event) override {
        if (std::holds_alternative<StartTransaction>(event)) return work(FileInfoWork{path_});
        if (const auto* drained = std::get_if<OutputDrained>(&event)) {
            if (offset_ == size_) { closing_ = true; return work(FileCloseWork{file_}); }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>({drained->body_capacity, size_ - offset_, 65536}));
            return work(FileReadWork{file_, offset_, count});
        }
        const auto& result = std::get<WorkEvent>(event);
        if (result.status != WorkStatus::complete && result.status != WorkStatus::progress) return failure(result.error);
        if (closing_) return ResponseFinished{};
        if (!info_received_) { info_received_ = true; return work(FileOpenWork{path_}); }
        if (!file_) {
            file_ = result.file; size_ = result.info.size;
            Response head;
            head.headers.set("Content-Type", "application/octet-stream");
            head.headers.set("Last-Modified", format_http_date(result.info.modified));
            return ResponseHead{std::move(head), BodyFraming::fixed_length, size_};
        }
        if (!result.bytes || result.bytes->empty() || result.bytes->size() > size_ - offset_) return failure();
        offset_ += result.bytes->size();
        return ResponseBody{result.bytes};
    }
private:
    TransactionWork work(WorkRequest request) { return {std::move(request), clock_.now() + std::chrono::seconds(5)}; }
    std::filesystem::path path_;
    const core::SteadyClock& clock_;
    FileToken file_;
    std::uint64_t size_{}, offset_{};
    bool info_received_{}, closing_{};
};

class OriginTransaction final : public Transaction {
public:
    OriginTransaction(net::HostPort origin, SharedBytes request, Method method, const TransactionContext& context)
        : origin_(std::move(origin)), request_(std::move(request)), method_(method), parser_(method),
          deadline_(context.steady_clock.now() + std::chrono::seconds(5)),
          capacity_(std::min<std::size_t>(context.response_window - 20, 65536)) {}
    TransactionStep on_event(const TransactionEvent& event) override {
        if (std::holds_alternative<StartTransaction>(event)) return work(ResolveWork{origin_});
        if (const auto* output = std::get_if<OutputDrained>(&event)) {
            capacity_ = std::min<std::size_t>(output->body_capacity, 65536);
            return pump();
        }
        const auto& result = std::get<WorkEvent>(event);
        if (result.status == WorkStatus::failed || result.status == WorkStatus::timed_out ||
            result.status == WorkStatus::cancelled || result.status == WorkStatus::rejected) return failure(result.error);
        if (state_ == State::resolve) { state_ = State::connect; return work(ConnectWork{result.candidates}); }
        if (state_ == State::connect) { state_ = State::write; return write(); }
        if (state_ == State::wait_write) { state_ = State::write; return write(); }
        if (state_ == State::write) {
            if (result.status == WorkStatus::would_block) { state_ = State::wait_write; return work(UpstreamWaitWork{net::Interest::write}); }
            if (result.status != WorkStatus::progress || result.transferred == 0 || result.transferred > request_->size() - sent_) return failure();
            sent_ += result.transferred;
            if (sent_ < request_->size()) return write();
            state_ = State::read; return read();
        }
        if (state_ == State::wait_read) { state_ = State::read; return read(); }
        if (state_ == State::closing) return ResponseFinished{parser_.response().trailers};
        if (result.status == WorkStatus::would_block) { state_ = State::wait_read; return work(UpstreamWaitWork{net::Interest::read}); }
        if (result.status == WorkStatus::eof) {
            if (!parser_.finish_on_eof()) return failure(parser_.error_detail());
        } else if (result.status == WorkStatus::progress && result.bytes && !result.bytes->empty()) {
            input_ = result.bytes; consumed_ = 0;
        } else return failure();
        return pump();
    }
private:
    enum class State { resolve, connect, write, wait_write, read, wait_read, closing };
    TransactionWork work(WorkRequest request) { return {std::move(request), deadline_}; }
    TransactionStep write() { return work(UpstreamWriteWork{request_, sent_, std::min<std::size_t>(request_->size() - sent_, 65536)}); }
    TransactionStep read() { input_.reset(); consumed_ = 0; return work(UpstreamReadWork{capacity_}); }
    TransactionStep pump() {
        for (;;) {
            const auto input = input_ ? std::span(*input_).subspan(consumed_) : std::span<const std::byte>{};
            const auto event = parser_.consume_stream(input, capacity_);
            consumed_ += event.consumed;
            if (event.kind == ResponseEventKind::informational_head || event.kind == ResponseEventKind::trailers) continue;
            if (event.kind == ResponseEventKind::error) return failure(parser_.error_detail());
            if (event.kind == ResponseEventKind::paused) return failure();
            if (event.kind == ResponseEventKind::need_more) return read();
            if (event.kind == ResponseEventKind::complete) { state_ = State::closing; return work(UpstreamCloseWork{}); }
            if (event.kind == ResponseEventKind::body) return ResponseBody{std::make_shared<const Bytes>(event.body.begin(), event.body.end())};
            if (event.kind == ResponseEventKind::final_head) {
                const auto& origin = parser_.response();
                Response head(origin.status_code, origin.reason);
                // This reference returns representation bytes/metadata, not a
                // general forward proxy. No downstream request fields forwarded.
                for (const auto* name : {"Content-Type", "Content-Encoding"}) {
                    if (auto value = origin.headers.get(name)) head.headers.set(name, std::string(*value));
                }
                if (method_ == Method::HEAD || origin.status_code == 204 || origin.status_code == 304) {
                    if (origin.headers.contains("Content-Length")) {
                        const auto framing = select_request_framing(origin.headers);
                        return ResponseHead{std::move(head), BodyFraming::fixed_length, framing.content_length};
                    }
                    return ResponseHead{std::move(head), BodyFraming::none, {}};
                }
                return ResponseHead{std::move(head), BodyFraming::chunked, {}};
            }
        }
    }
    net::HostPort origin_;
    SharedBytes request_, input_;
    Method method_;
    ResponseParser parser_;
    core::SteadyTimePoint deadline_;
    std::size_t capacity_, sent_{}, consumed_{};
    State state_{State::resolve};
};
class FileFactory final : public TransactionFactory {
public:
    explicit FileFactory(std::filesystem::path path) : path_(std::move(path)) {}
    core::Result<std::unique_ptr<Transaction>> create(const Request&, const TransactionContext& context) const override {
        return std::unique_ptr<Transaction>(std::make_unique<FileTransaction>(path_, context));
    }
private:
    std::filesystem::path path_;
};
class OriginFactory final : public TransactionFactory {
public:
    OriginFactory(net::HostPort origin, std::string target) : origin_(std::move(origin)), target_(std::move(target)) {}
    core::Result<std::unique_ptr<Transaction>> create(const Request& client, const TransactionContext& context) const override {
        if (context.response_window <= 20) return core::make_error(core::ErrorCategory::invalid_argument, "origin reference window");
        Request request; request.method = client.method; request.target = target_;
        request.headers.set("Host", origin_.to_string()); request.headers.set("Connection", "close");
        auto wire = serialize_request_head(request);
        if (!wire) return wire.error();
        return std::unique_ptr<Transaction>(std::make_unique<OriginTransaction>(origin_, bytes(wire.value()), client.method, context));
    }
private:
    net::HostPort origin_; std::string target_;
};
}
std::shared_ptr<const TransactionFactory> make_file_reference_factory(std::filesystem::path path) { return std::make_shared<FileFactory>(std::move(path)); }
std::shared_ptr<const TransactionFactory> make_origin_reference_factory(net::HostPort origin, std::string target) { return std::make_shared<OriginFactory>(std::move(origin), std::move(target)); }
} // namespace http
