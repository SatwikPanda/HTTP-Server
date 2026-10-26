#pragma once

#include "cache/contracts.hpp"

#include <memory>

namespace cache::storage {

struct Limits {
    std::size_t entries{1024};
    std::size_t resident_bytes{64 * 1024 * 1024};
    std::size_t total_bytes{128 * 1024 * 1024};
    std::size_t object_bytes{8 * 1024 * 1024};
    std::size_t readers{4096};
    std::size_t metadata_bytes{64 * 1024};
};

struct Statistics {
    std::size_t entries{}, resident_bytes{}, total_bytes{};
    std::size_t candidate_bytes{}, retained_bytes{}, reader_bytes{}, readers{};
};

class Candidate {
public:
    ~Candidate();
    Candidate(Candidate&&) noexcept;
    Candidate& operator=(Candidate&&) noexcept;
    Candidate(const Candidate&) = delete;
    Candidate& operator=(const Candidate&) = delete;
    // On failure existing bytes remain intact. Destroy the builder to abort.
    [[nodiscard]] core::Result<void> append(std::span<const std::byte> bytes);
    [[nodiscard]] core::Result<Snapshot> finish();
private:
    friend class LruStorage;
    struct Impl;
    explicit Candidate(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class LruStorage final : public Storage {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<LruStorage>> create(Limits limits = {});
    ~LruStorage() override;
    LruStorage(const LruStorage&) = delete;
    LruStorage& operator=(const LruStorage&) = delete;

    // Inputs are borrowed only during this call. Body/framing are not copied.
    // Supply sanitized end-to-end headers; the store rejects hop/framing fields.
    [[nodiscard]] core::Result<Candidate> begin(const Key&, const http::HeaderMap&, Timing, int status = 200);
    // A new metadata allocation shares the exact selected immutable body.
    [[nodiscard]] core::Result<Snapshot> refresh(const Snapshot&, const http::HeaderMap&, Timing);
    [[nodiscard]] LookupResult lookup(const Key&) override;
    [[nodiscard]] StoreResult replace(const Key&, EntryVersion expected, Snapshot candidate) override;
    [[nodiscard]] StoreResult erase(const Key&, EntryVersion expected) override;
    [[nodiscard]] core::Result<void> clear();
    [[nodiscard]] Statistics statistics() const;
private:
    struct Impl;
    explicit LruStorage(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cache::storage
