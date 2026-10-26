#include "resource_probe.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <processsnapshot.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <unistd.h>
#include <vector>
#elif defined(__linux__)
#include <filesystem>
#endif

namespace test {
ResourceSample resource_sample() {
    ResourceSample sample;
#if defined(_WIN32)
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) sample.handles = handles;
    // Optional diagnosis of process-global runtime handles, without exposing
    // object names or walking another process. Snapshot only handles, never VA.
    using Capture = DWORD(WINAPI*)(HANDLE, PSS_CAPTURE_FLAGS, DWORD, HPSS*);
    using Free = DWORD(WINAPI*)(HANDLE, HPSS);
    using Marker = DWORD(WINAPI*)(const PSS_ALLOCATOR*, HPSSWALK*);
    using FreeMarker = DWORD(WINAPI*)(HPSSWALK);
    using Walk = DWORD(WINAPI*)(HPSS, PSS_WALK_INFORMATION_CLASS, HPSSWALK, void*, DWORD);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto capture = reinterpret_cast<Capture>(GetProcAddress(kernel, "PssCaptureSnapshot"));
    const auto release = reinterpret_cast<Free>(GetProcAddress(kernel, "PssFreeSnapshot"));
    const auto marker_create = reinterpret_cast<Marker>(GetProcAddress(kernel, "PssWalkMarkerCreate"));
    const auto marker_free = reinterpret_cast<FreeMarker>(GetProcAddress(kernel, "PssWalkMarkerFree"));
    const auto walk = reinterpret_cast<Walk>(GetProcAddress(kernel, "PssWalkSnapshot"));
    HPSS captured{};
    if (capture && release && marker_create && marker_free && walk &&
        capture(GetCurrentProcess(), PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION, 0, &captured) == ERROR_SUCCESS) {
        HPSSWALK marker{};
        if (marker_create(nullptr, &marker) == ERROR_SUCCESS) {
            PSS_HANDLE_ENTRY entry{};
            while (walk(captured, PSS_WALK_HANDLES, marker, &entry, sizeof(entry)) == ERROR_SUCCESS) {
                std::string type;
                for (unsigned i = 0; entry.TypeName && i < entry.TypeNameLength / sizeof(wchar_t) && entry.TypeName[i]; ++i) {
                    type += static_cast<char>(entry.TypeName[i]);
                }
                ++sample.handle_types[type];
            }
            marker_free(marker);
        }
        release(GetCurrentProcess(), captured);
    }
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        std::size_t count = 0;
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
            } while (Thread32Next(snapshot, &entry));
            sample.threads = count;
        }
        CloseHandle(snapshot);
    }
#elif defined(__APPLE__)
    // proc_pidinfo returns bytes and may require an extra slot if a descriptor
    // opens concurrently. Samples are taken only after application quiescence.
    const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes > 0) {
        std::vector<proc_fdinfo> descriptors(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
        const auto used = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, descriptors.data(),
            static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
        if (used > 0) sample.handles = static_cast<std::size_t>(used) / sizeof(proc_fdinfo);
    }
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS) {
        sample.threads = count;
        for (mach_msg_type_number_t i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
    }
#elif defined(__linux__)
    auto count = [](const char* path) -> std::optional<std::size_t> {
        std::error_code error;
        std::filesystem::directory_iterator it(path, error), end;
        if (error) return std::nullopt;
        std::size_t size = 0;
        for (; it != end; it.increment(error)) {
            if (error) return std::nullopt;
            ++size;
        }
        if (error) return std::nullopt;
        return size;
    };
    sample.handles = count("/proc/self/fd");
    sample.threads = count("/proc/self/task");
#endif
    return sample;
}
}
