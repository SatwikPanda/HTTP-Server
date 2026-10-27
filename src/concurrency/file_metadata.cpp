#include "detail/file_metadata.hpp"

#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace concurrency::detail {
core::Result<FileInfo> file_metadata(const std::filesystem::path& path, FileFailure& failure) {
    std::error_code error;
    auto fail = [&](std::string operation) {
        // A path below a regular file (not_a_directory) names nothing: missing, not an I/O fault.
        failure = (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) ? FileFailure::missing :
                  error == std::errc::permission_denied ? FileFailure::forbidden : FileFailure::io;
        return core::make_error(core::ErrorCategory::io_error, std::move(operation), error.value());
    };
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return fail("file absolute path");
    FileInfo info;
    auto prefix = absolute.root_path();
    const auto components = absolute.relative_path();
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA final_attributes{};
#endif
    for (auto component = components.begin(); component != components.end(); ++component) {
        prefix /= *component;
#ifdef _WIN32
        // The final component query also supplies type and size; do not issue
        // separate status/file_size calls for data already returned by Windows.
        const auto attributes = std::next(component) == components.end()
            ? (::GetFileAttributesExW(prefix.c_str(), GetFileExInfoStandard, &final_attributes)
                ? final_attributes.dwFileAttributes : INVALID_FILE_ATTRIBUTES)
            : ::GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = ::GetLastError();
            error = std::error_code(static_cast<int>(code), std::system_category());
            return fail("file attributes");
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) { info.has_link_component = true; return info; }
#else
        const auto status = std::filesystem::symlink_status(prefix, error);
        if (error) return fail("file link status");
        if (std::filesystem::is_symlink(status)) { info.has_link_component = true; return info; }
#endif
    }
#ifdef _WIN32
    if (components.empty() && !::GetFileAttributesExW(absolute.c_str(), GetFileExInfoStandard, &final_attributes)) {
        const auto code = ::GetLastError();
        error = std::error_code(static_cast<int>(code), std::system_category());
        return fail("file attributes");
    }
    info.directory = (final_attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.regular = !info.directory; // all reparse components were rejected above
#else
    const auto status = std::filesystem::status(absolute, error);
    if (error) return fail("file status");
    if (!std::filesystem::exists(status)) {
        error = std::make_error_code(std::errc::no_such_file_or_directory); return fail("file status");
    }
    info.regular = std::filesystem::is_regular_file(status);
    info.directory = std::filesystem::is_directory(status);
#endif
    info.canonical_path = std::filesystem::canonical(absolute, error);
    if (error) return fail("file canonical path");
    if (info.regular) {
#ifdef _WIN32
        info.size = (static_cast<std::uint64_t>(final_attributes.nFileSizeHigh) << 32) |
                    final_attributes.nFileSizeLow;
#else
        info.size = std::filesystem::file_size(absolute, error);
        if (error) return fail("file size");
#endif
    }
    const auto modified = std::filesystem::last_write_time(absolute, error);
    if (error) return fail("file time");
#ifdef _MSC_VER
    // The MS STL exposes file_clock::to_utc, while libstdc++ uses to_sys.
    info.modified = std::chrono::time_point_cast<core::SystemTimePoint::duration>(std::chrono::clock_cast<std::chrono::system_clock>(modified));
#else
    info.modified = std::chrono::time_point_cast<core::SystemTimePoint::duration>(std::chrono::file_clock::to_sys(modified));
#endif
    return info;
}
} // namespace concurrency::detail
