#include "observability/process_sampler.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#endif
#endif

namespace observability {
namespace {

#ifdef _WIN32
std::chrono::microseconds filetime_to_us(const FILETIME& ft) noexcept {
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // FILETIME is in 100-nanosecond intervals.
    return std::chrono::microseconds(static_cast<std::int64_t>(uli.QuadPart / 10ULL));
}
#endif

ProcessSample query_os_sample(core::SteadyTimePoint now) noexcept {
    ProcessSample sample{};
    sample.timestamp = now;

#ifdef _WIN32
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user) != FALSE) {
        const auto user_us = filetime_to_us(user);
        const auto sys_us = filetime_to_us(kernel);
        sample.user_cpu_time = user_us;
        sample.system_cpu_time = sys_us;
        sample.total_cpu_time = user_us + sys_us;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) != FALSE) {
        sample.resident_memory_bytes = static_cast<std::size_t>(pmc.WorkingSetSize);
        sample.peak_resident_memory_bytes = static_cast<std::size_t>(pmc.PeakWorkingSetSize);
    }
#elif defined(__unix__) || defined(__APPLE__)
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        const auto user_us = std::chrono::seconds(usage.ru_utime.tv_sec) +
                             std::chrono::microseconds(usage.ru_utime.tv_usec);
        const auto sys_us = std::chrono::seconds(usage.ru_stime.tv_sec) +
                            std::chrono::microseconds(usage.ru_stime.tv_usec);
        sample.user_cpu_time = user_us;
        sample.system_cpu_time = sys_us;
        sample.total_cpu_time = user_us + sys_us;
#if defined(__APPLE__)
        if (usage.ru_maxrss > 0) {
            sample.peak_resident_memory_bytes = static_cast<std::size_t>(usage.ru_maxrss);
        }
#else
        if (usage.ru_maxrss > 0) {
            sample.peak_resident_memory_bytes = static_cast<std::size_t>(usage.ru_maxrss) * 1024ULL;
        }
#endif
    }
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        sample.resident_memory_bytes = static_cast<std::size_t>(info.resident_size);
    }
#elif defined(__linux__)
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long total_pages = 0;
        unsigned long rss_pages = 0;
        if (std::fscanf(f, "%lu %lu", &total_pages, &rss_pages) == 2) {
            const long page_size = ::sysconf(_SC_PAGESIZE);
            if (page_size > 0) {
                sample.resident_memory_bytes = static_cast<std::size_t>(rss_pages) * static_cast<std::size_t>(page_size);
            }
        }
        std::fclose(f);
    }
#endif
#endif
    return sample;
}

} // namespace

ProcessSampler::ProcessSampler(std::chrono::milliseconds min_interval, const core::SteadyClock& clock) noexcept
    : min_interval_(std::max(std::chrono::milliseconds::zero(), min_interval)),
      clock_(&clock) {}

ProcessSample ProcessSampler::sample() noexcept {
    std::lock_guard lock(mutex_);
    const auto now = clock_->now();
    if (has_last_ && min_interval_ > std::chrono::milliseconds::zero() &&
        now >= last_sample_.timestamp && (now - last_sample_.timestamp) < min_interval_) {
        return last_sample_;
    }

    auto current = query_os_sample(now);
    if (has_last_ && current.total_cpu_time && last_sample_.total_cpu_time && now > last_sample_.timestamp) {
        const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_sample_.timestamp).count();
        const auto cpu_delta = (*current.total_cpu_time - *last_sample_.total_cpu_time).count();
        if (wall_us > 0 && cpu_delta >= 0) {
            current.cpu_utilization_ratio = static_cast<double>(cpu_delta) / static_cast<double>(wall_us);
        }
    }
    last_sample_ = current;
    has_last_ = true;
    return current;
}

ProcessSample ProcessSampler::force_sample() noexcept {
    std::lock_guard lock(mutex_);
    const auto now = clock_->now();
    auto current = query_os_sample(now);
    if (has_last_ && current.total_cpu_time && last_sample_.total_cpu_time && now > last_sample_.timestamp) {
        const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_sample_.timestamp).count();
        const auto cpu_delta = (*current.total_cpu_time - *last_sample_.total_cpu_time).count();
        if (wall_us > 0 && cpu_delta >= 0) {
            current.cpu_utilization_ratio = static_cast<double>(cpu_delta) / static_cast<double>(wall_us);
        }
    }
    last_sample_ = current;
    has_last_ = true;
    return current;
}

} // namespace observability
