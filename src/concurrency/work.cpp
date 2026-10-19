#include "concurrency/work.hpp"
#include "detail/file_metadata.hpp"

#include "net/resolver.hpp"
#include "net/socket.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <type_traits>

namespace concurrency {
class FileResource {
public:
    std::mutex mutex;
    std::ifstream stream;
};
struct ReferenceWorkExecutor::Impl {
    WorkLimits limits;
    net::Socket upstream;
    core::TransactionId owner{};
    std::uint64_t last_operation{};
    bool busy{};
};
core::EndpointRole endpoint_role(const WorkRequest& request) noexcept {
    return std::visit([](const auto& work) {
        using T = std::decay_t<decltype(work)>;
        if constexpr (std::is_same_v<T, ResolveWork>) return core::EndpointRole::resolver;
        else if constexpr (std::is_same_v<T, FileInfoWork> || std::is_same_v<T, FileOpenWork> ||
                           std::is_same_v<T, FileReadWork> || std::is_same_v<T, FileCloseWork>) return core::EndpointRole::file;
        else return core::EndpointRole::upstream;
    }, request);
}
ReferenceWorkExecutor::ReferenceWorkExecutor(WorkLimits limits) : impl_(std::make_unique<Impl>()) { impl_->limits = limits; }
ReferenceWorkExecutor::~ReferenceWorkExecutor() = default;
core::Result<void> ReferenceWorkExecutor::submit(WorkAction action, Completion completion) {
    if (impl_->busy) return core::make_error(core::ErrorCategory::would_block, "work admission");
    if (!completion) return core::make_error(core::ErrorCategory::invalid_argument, "work completion");
    struct Guard { bool& busy; ~Guard() { busy = false; } } guard{impl_->busy};
    impl_->busy = true;
    completion(execute(action));
    return {};
}
WorkEvent ReferenceWorkExecutor::execute(const WorkAction& action, const std::function<bool()>& stop) {
    WorkEvent result; result.id = action.id;
    auto fail = [&](core::Error error, WorkStatus status = WorkStatus::failed) {
        result.error = std::move(error); result.status = status;
    };
    auto interrupted = [&] {
        if (stop() || (action.cancellation && action.cancellation->is_cancelled())) {
            result.status = WorkStatus::cancelled; return true;
        }
        if (std::chrono::steady_clock::now() >= action.deadline) {
            fail(core::make_error(core::ErrorCategory::timed_out, "work deadline"), WorkStatus::timed_out); return true;
        }
        return false;
    };
    const auto invalid = [] { return core::make_error(core::ErrorCategory::invalid_argument, "work request"); };
    if (impl_->limits.max_buffer_bytes == 0 || impl_->limits.max_buffer_bytes > 16 * 1024 * 1024 ||
        impl_->limits.max_candidates == 0 || impl_->limits.max_candidates > 256 ||
        impl_->limits.max_path_bytes == 0 || impl_->limits.max_path_bytes > 32768) {
        fail(invalid(), WorkStatus::rejected); return result;
    }
    if (action.id.transaction.sequence == 0 || action.id.sequence == 0 || action.id.endpoint != endpoint_role(action.request)) {
        fail(invalid()); return result;
    }
    // A connection serves one transaction at a time. A successor implicitly
    // releases its predecessor's upstream even if the application forgot close.
    if (impl_->owner.sequence != 0 &&
        (impl_->owner.connection != action.id.transaction.connection ||
         action.id.transaction.sequence < impl_->owner.sequence ||
         (action.id.transaction == impl_->owner && action.id.sequence <= impl_->last_operation))) {
        result.status = WorkStatus::cancelled;
        return result;
    }
    if (impl_->owner != action.id.transaction) {
        impl_->upstream.close(); impl_->owner = action.id.transaction;
    }
    impl_->last_operation = action.id.sequence;
    if (interrupted()) { impl_->upstream.close(); return result; }
    auto wait = [&](net::Socket& socket, net::Interest interest) -> bool {
        auto created = net::Poller::create();
        if (!created) { fail(created.error()); return false; }
        auto poller = std::move(created.value());
        auto watched = poller.watch(socket, interest, action.id.transaction.connection);
        if (!watched) { fail(watched.error()); return false; }
        struct Guard { net::Poller& poller; net::RegistrationToken token; ~Guard() { (void)poller.unwatch(token); } } guard{poller, watched.value()};
        while (!interrupted()) {
            const auto deadline = std::min(action.deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(25));
            auto events = poller.wait(deadline);
            if (!events) { fail(events.error()); return false; }
            for (const auto& event : events.value()) {
                if (event.token == watched.value() && poller.is_current(event)) return true;
            }
        }
        return false;
    };
    std::visit([&](const auto& work) {
        using T = std::decay_t<decltype(work)>;
        if constexpr (std::is_same_v<T, ResolveWork>) {
            if (work.target.host.size() > impl_->limits.max_path_bytes) { fail(invalid(), WorkStatus::rejected); return; }
            auto resolved = net::resolve(work.target);
            if (!resolved) { fail(resolved.error()); return; }
            if (resolved.value().size() > impl_->limits.max_candidates) { fail(invalid(), WorkStatus::rejected); return; }
            result.candidates = std::move(resolved.value());
        } else if constexpr (std::is_same_v<T, ConnectWork>) {
            impl_->upstream.close();
            if (work.candidates.empty() || work.candidates.size() > impl_->limits.max_candidates) { fail(invalid(), WorkStatus::rejected); return; }
            for (const auto& candidate : work.candidates) {
                if (interrupted()) return;
                auto created = net::create_tcp_socket(candidate.family());
                if (!created) { fail(created.error()); continue; }
                auto socket = std::move(created.value());
                auto begun = socket.begin_connect(candidate);
                if (!begun) { fail(begun.error()); continue; }
                bool connected = begun.value() == net::ConnectState::connected;
                while (!connected && wait(socket, net::Interest::write)) {
                    auto finished = socket.finish_connect();
                    if (finished) connected = true;
                    else if (finished.error().category != core::ErrorCategory::would_block) { fail(finished.error()); break; }
                }
                if (connected && !interrupted()) {
                    impl_->upstream = std::move(socket); result.status = WorkStatus::complete; result.error = {}; return;
                }
            }
        } else if constexpr (std::is_same_v<T, UpstreamReadWork>) {
            if (work.max_bytes == 0 || work.max_bytes > impl_->limits.max_buffer_bytes) { fail(invalid(), WorkStatus::rejected); return; }
            auto bytes = std::make_shared<Bytes>(work.max_bytes);
            const auto read = impl_->upstream.read_some(*bytes);
            if (read.is_ok() && read.bytes_transferred > 0 && read.bytes_transferred <= bytes->size()) {
                bytes->resize(read.bytes_transferred); result.bytes = std::move(bytes); result.transferred = read.bytes_transferred; result.status = WorkStatus::progress;
            } else if (read.is_eof()) result.status = WorkStatus::eof;
            else if (read.would_block()) result.status = WorkStatus::would_block;
            else fail(read.is_ok() ? invalid() : read.error);
        } else if constexpr (std::is_same_v<T, UpstreamWriteWork>) {
            if (!work.bytes || work.count == 0 || work.count > impl_->limits.max_buffer_bytes ||
                work.offset > work.bytes->size() || work.count > work.bytes->size() - work.offset) { fail(invalid(), WorkStatus::rejected); return; }
            const auto wrote = impl_->upstream.write_some(std::span(*work.bytes).subspan(work.offset, work.count));
            if (wrote.is_ok() && wrote.bytes_transferred > 0 && wrote.bytes_transferred <= work.count) {
                result.transferred = wrote.bytes_transferred; result.status = WorkStatus::progress;
            } else if (wrote.would_block()) result.status = WorkStatus::would_block;
            else fail(wrote.is_ok() ? invalid() : wrote.error);
        } else if constexpr (std::is_same_v<T, UpstreamWaitWork>) {
            if (work.interest != net::Interest::read && work.interest != net::Interest::write) { fail(invalid()); return; }
            (void)wait(impl_->upstream, work.interest);
        } else if constexpr (std::is_same_v<T, UpstreamCloseWork>) {
            impl_->upstream.close();
        } else if constexpr (std::is_same_v<T, FileInfoWork> || std::is_same_v<T, FileOpenWork>) {
            const auto path_text = work.path.native();
            if (path_text.empty() || path_text.size() > impl_->limits.max_path_bytes || path_text.find(typename std::filesystem::path::value_type{}) != path_text.npos) {
                fail(invalid(), WorkStatus::rejected); return;
            }
            auto metadata = detail::file_metadata(work.path, result.file_failure);
            if (!metadata) { fail(metadata.error()); return; }
            result.info = std::move(metadata.value());
            if constexpr (std::is_same_v<T, FileOpenWork>) {
                if (result.info.has_link_component || !result.info.regular) {
                    result.file_failure = result.info.has_link_component ? FileFailure::forbidden : FileFailure::not_regular;
                    fail(core::make_error(core::ErrorCategory::invalid_argument, "file open kind")); return;
                }
                auto file = std::make_shared<FileResource>();
                file->stream.open(work.path, std::ios::binary | std::ios::ate);
                const auto size = file->stream.tellg();
                if (!file->stream || size < 0) { fail(core::make_error(core::ErrorCategory::io_error, "file open")); return; }
                result.info.size = static_cast<std::uint64_t>(size);
                result.file.resource_ = std::move(file);
            }
        } else if constexpr (std::is_same_v<T, FileReadWork>) {
            if (!work.file || work.max_bytes == 0 || work.max_bytes > impl_->limits.max_buffer_bytes ||
                work.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) { fail(invalid(), WorkStatus::rejected); return; }
            auto file = work.file.resource_; // pins the stream throughout the call
            std::lock_guard lock(file->mutex);
            if (!file->stream.is_open()) { fail(core::make_error(core::ErrorCategory::bad_descriptor, "file read")); return; }
            file->stream.clear(); file->stream.seekg(static_cast<std::streamoff>(work.offset));
            if (!file->stream) { fail(core::make_error(core::ErrorCategory::io_error, "file seek")); return; }
            auto bytes = std::make_shared<Bytes>(work.max_bytes);
            file->stream.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            const auto count = file->stream.gcount();
            if (file->stream.bad() || (file->stream.fail() && !file->stream.eof())) { fail(core::make_error(core::ErrorCategory::io_error, "file read")); return; }
            if (count == 0) { result.status = WorkStatus::eof; return; }
            bytes->resize(static_cast<std::size_t>(count)); result.transferred = bytes->size(); result.bytes = std::move(bytes); result.status = WorkStatus::progress;
        } else if constexpr (std::is_same_v<T, FileCloseWork>) {
            if (work.file) { auto file = work.file.resource_; std::lock_guard lock(file->mutex); file->stream.close(); }
        }
    }, action.request);
    if (interrupted()) {
        impl_->upstream.close(); result.bytes.reset(); result.file = {}; result.candidates.clear(); result.transferred = 0;
    }
    return result;
}
} // namespace concurrency
