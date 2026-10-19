#pragma once

#include "core/result.hpp"

#include <cstddef>
#include <memory>
#include <mutex>

namespace core {

enum class AllocationKind { candidate, resident, pinned, temporary, metadata };
struct BudgetState;

// A move transfers the charge; sharing the owner does not charge it again.
// Reclassifying never releases/reacquires capacity (no transient oversubscription).
class Reservation {
public:
    Reservation() = default;
    ~Reservation();
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    [[nodiscard]] Result<void> resize(std::size_t bytes);
    void reclassify(AllocationKind kind) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return bytes_; }
    [[nodiscard]] AllocationKind kind() const noexcept { return kind_; }
private:
    friend class MemoryBudget;
    Reservation(std::shared_ptr<BudgetState> state, std::size_t bytes, AllocationKind kind);
    void release() noexcept;
    std::shared_ptr<BudgetState> state_;
    std::size_t bytes_{};
    AllocationKind kind_{AllocationKind::candidate};
};

class MemoryBudget {
public:
    explicit MemoryBudget(std::size_t capacity);
    [[nodiscard]] Result<Reservation> reserve(std::size_t bytes, AllocationKind kind = AllocationKind::candidate);
    [[nodiscard]] std::size_t used() const noexcept;
    [[nodiscard]] std::size_t used(AllocationKind kind) const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
private:
    std::shared_ptr<BudgetState> state_;
};
} // namespace core
