#pragma once

#include "core/clock.hpp"
#include "core/id.hpp"
#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/poller.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace concurrency {

using Bytes = std::vector<std::byte>;
using SharedBytes = std::shared_ptr<const Bytes>;
class FileResource;
class FileToken {
public:
    FileToken() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(resource_); }
private:
    friend class ReferenceWorkExecutor;
    std::shared_ptr<FileResource> resource_;
};
struct FileInfo {
    std::uint64_t size{};
    core::SystemTimePoint modified{};
    bool regular{}, directory{}, has_link_component{};
    std::filesystem::path canonical_path;
};
enum class FileFailure { none, missing, forbidden, not_regular, io };
struct ResolveWork { net::HostPort target; };
struct ConnectWork { std::vector<net::Endpoint> candidates; };
struct UpstreamReadWork { std::size_t max_bytes{}; };
struct UpstreamWriteWork { SharedBytes bytes; std::size_t offset{}; std::size_t count{}; };
struct UpstreamWaitWork { net::Interest interest{net::Interest::read}; };
struct UpstreamCloseWork {};
struct FileInfoWork { std::filesystem::path path; };
struct FileOpenWork { std::filesystem::path path; };
struct FileReadWork { FileToken file; std::uint64_t offset{}; std::size_t max_bytes{}; };
struct FileCloseWork { FileToken file; };
using WorkRequest = std::variant<ResolveWork, ConnectWork, UpstreamReadWork, UpstreamWriteWork,
    UpstreamWaitWork, UpstreamCloseWork, FileInfoWork, FileOpenWork, FileReadWork, FileCloseWork>;
[[nodiscard]] core::EndpointRole endpoint_role(const WorkRequest&) noexcept;

struct Cancellation {
    void cancel() noexcept { cancelled.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled.load(std::memory_order_acquire); }
private:
    std::atomic<bool> cancelled{false};
};
struct WorkAction {
    core::OperationId id;
    core::SteadyTimePoint deadline;
    WorkRequest request;
    std::shared_ptr<Cancellation> cancellation;
};
enum class WorkStatus { complete, progress, would_block, eof, cancelled, timed_out, rejected, failed };
struct WorkEvent {
    core::OperationId id;
    WorkStatus status{WorkStatus::complete};
    core::Error error;
    std::vector<net::Endpoint> candidates;
    SharedBytes bytes;
    std::size_t transferred{};
    FileToken file;
    FileInfo info;
    FileFailure file_failure{FileFailure::none};
};
struct WorkLimits {
    std::size_t max_buffer_bytes{64 * 1024};
    std::size_t max_candidates{32};
    std::size_t max_path_bytes{8192};
};

// Successful admission delivers exactly one owning result, possibly inline.
// Failed admission delivers no callback. Async implementations queue callbacks
// to the owning driver; they must never call ConnectionSession on worker threads.
class WorkExecutor {
public:
    using Completion = std::function<void(WorkEvent)>;
    virtual ~WorkExecutor() = default;
    [[nodiscard]] virtual core::Result<void> submit(WorkAction action, Completion completion) = 0;
};

// Single-owner, synchronous reference. Never use submit on an event-loop thread.
// DNS/file calls are not interruptible; cancellation/deadline checked afterwards.
class ReferenceWorkExecutor final : public WorkExecutor {
public:
    explicit ReferenceWorkExecutor(WorkLimits limits = {});
    ~ReferenceWorkExecutor() override;
    [[nodiscard]] core::Result<void> submit(WorkAction action, Completion completion) override;
    [[nodiscard]] WorkEvent execute(const WorkAction& action, const std::function<bool()>& stop = [] { return false; });
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace concurrency
