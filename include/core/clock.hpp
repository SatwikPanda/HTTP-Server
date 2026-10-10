#pragma once

#include <chrono>

namespace core {

using SteadyTimePoint = std::chrono::steady_clock::time_point;
using SystemTimePoint = std::chrono::system_clock::time_point;

class SteadyClock {
public:
    virtual ~SteadyClock() = default;
    [[nodiscard]] virtual SteadyTimePoint now() const noexcept = 0;
};

class SystemClock {
public:
    virtual ~SystemClock() = default;
    [[nodiscard]] virtual SystemTimePoint now() const noexcept = 0;
};

class RealSteadyClock final : public SteadyClock {
public:
    [[nodiscard]] SteadyTimePoint now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

class RealSystemClock final : public SystemClock {
public:
    [[nodiscard]] SystemTimePoint now() const noexcept override {
        return std::chrono::system_clock::now();
    }
};

class ManualSteadyClock final : public SteadyClock {
public:
    explicit ManualSteadyClock(SteadyTimePoint start = SteadyTimePoint{}) noexcept
        : current_(start) {}

    [[nodiscard]] SteadyTimePoint now() const noexcept override {
        return current_;
    }

    void advance(std::chrono::nanoseconds duration) noexcept {
        current_ += duration;
    }

    void set(SteadyTimePoint tp) noexcept {
        current_ = tp;
    }

private:
    SteadyTimePoint current_;
};

class ManualSystemClock final : public SystemClock {
public:
    explicit ManualSystemClock(SystemTimePoint start = SystemTimePoint{}) noexcept
        : current_(start) {}

    [[nodiscard]] SystemTimePoint now() const noexcept override {
        return current_;
    }

    void advance(std::chrono::nanoseconds duration) noexcept {
        current_ += duration;
    }

    void set(SystemTimePoint tp) noexcept {
        current_ = tp;
    }

private:
    SystemTimePoint current_;
};

[[nodiscard]] inline const RealSteadyClock& real_steady_clock() noexcept {
    static const RealSteadyClock clock;
    return clock;
}

[[nodiscard]] inline const RealSystemClock& real_system_clock() noexcept {
    static const RealSystemClock clock;
    return clock;
}

} // namespace core
