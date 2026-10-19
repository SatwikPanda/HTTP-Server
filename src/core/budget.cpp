#include "core/budget.hpp"

#include <array>
#include <utility>

namespace core {
struct BudgetState {
    mutable std::mutex mutex;
    std::size_t capacity{}, used{};
    std::array<std::size_t, 5> classes{};
};
namespace {
std::size_t index(AllocationKind kind) { return static_cast<std::size_t>(kind); }
Error exhausted() { return make_error(ErrorCategory::would_block, "memory budget", 0, "Capacity exhausted"); }
}
MemoryBudget::MemoryBudget(std::size_t capacity) : state_(std::make_shared<BudgetState>()) { state_->capacity = capacity; }
Result<Reservation> MemoryBudget::reserve(std::size_t bytes, AllocationKind kind) {
    if (index(kind) >= state_->classes.size()) return make_error(ErrorCategory::invalid_argument, "memory budget class");
    std::lock_guard lock(state_->mutex);
    if (bytes > state_->capacity - state_->used) return exhausted();
    state_->used += bytes;
    state_->classes[index(kind)] += bytes;
    return Reservation(state_, bytes, kind);
}
std::size_t MemoryBudget::used() const noexcept { std::lock_guard lock(state_->mutex); return state_->used; }
std::size_t MemoryBudget::used(AllocationKind kind) const noexcept {
    std::lock_guard lock(state_->mutex);
    return index(kind) < state_->classes.size() ? state_->classes[index(kind)] : 0;
}
std::size_t MemoryBudget::capacity() const noexcept { return state_->capacity; }
Reservation::Reservation(std::shared_ptr<BudgetState> state, std::size_t bytes, AllocationKind kind)
    : state_(std::move(state)), bytes_(bytes), kind_(kind) {}
Reservation::~Reservation() { release(); }
Reservation::Reservation(Reservation&& other) noexcept
    : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)), kind_(other.kind_) {}
Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) { release(); state_ = std::move(other.state_); bytes_ = std::exchange(other.bytes_, 0); kind_ = other.kind_; }
    return *this;
}
void Reservation::release() noexcept {
    if (!state_) return;
    { std::lock_guard lock(state_->mutex); state_->used -= bytes_; state_->classes[index(kind_)] -= bytes_; }
    state_.reset(); bytes_ = 0;
}
Result<void> Reservation::resize(std::size_t bytes) {
    if (!state_) return make_error(ErrorCategory::invalid_argument, "empty reservation");
    std::lock_guard lock(state_->mutex);
    if (bytes > bytes_ && bytes - bytes_ > state_->capacity - state_->used) return exhausted();
    state_->used = state_->used - bytes_ + bytes;
    state_->classes[index(kind_)] = state_->classes[index(kind_)] - bytes_ + bytes;
    bytes_ = bytes;
    return {};
}
void Reservation::reclassify(AllocationKind kind) noexcept {
    if (!state_ || index(kind) >= state_->classes.size()) return;
    std::lock_guard lock(state_->mutex);
    state_->classes[index(kind_)] -= bytes_; state_->classes[index(kind)] += bytes_; kind_ = kind;
}
} // namespace core
