#pragma once

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace server_test {
// Owns a real temporary document tree. Track A keeps its own fixture so it does
// not depend on edits to the shared P0 fixture; the root is already canonical.
class Tree {
public:
    Tree() {
        static std::atomic<unsigned> serial{};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            root_ = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                    ("httpserver-static-" + std::to_string(test::Clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            std::error_code error;
            if (std::filesystem::create_directory(root_, error)) return;
        }
        CHECK(false);
    }
    ~Tree() { std::error_code error; std::filesystem::remove_all(root_, error); }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    std::filesystem::path write(const std::filesystem::path& relative, std::string_view bytes) {
        const auto path = root_ / relative;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        CHECK(!error);
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        CHECK(stream);
        return path;
    }
    std::filesystem::path make_directory(const std::filesystem::path& relative) {
        const auto path = root_ / relative;
        std::error_code error;
        std::filesystem::create_directories(path, error);
        CHECK(!error);
        return path;
    }
    // Sets a file's modification time from whole seconds since the Unix epoch
    // (tests control timestamps instead of sleeping).
    void set_mtime(const std::filesystem::path& path, std::chrono::system_clock::time_point when) {
        std::error_code error;
#ifdef _MSC_VER
        const auto native = std::chrono::clock_cast<std::chrono::file_clock>(when);
#else
        const auto native = std::chrono::file_clock::from_sys(when);
#endif
        std::filesystem::last_write_time(path, native, error);
        CHECK(!error);
    }
    // Returns false when the platform or account cannot create symbolic links.
    [[nodiscard]] bool try_symlink(const std::filesystem::path& target, const std::filesystem::path& link_relative) {
        std::error_code error;
        std::filesystem::create_symlink(target, root_ / link_relative, error);
        return !error;
    }
    [[nodiscard]] bool try_directory_symlink(const std::filesystem::path& target, const std::filesystem::path& link_relative) {
        std::error_code error;
        std::filesystem::create_directory_symlink(target, root_ / link_relative, error);
        return !error;
    }

private:
    std::filesystem::path root_;
};
} // namespace server_test
