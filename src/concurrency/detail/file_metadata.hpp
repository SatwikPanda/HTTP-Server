#pragma once

#include "concurrency/work.hpp"

namespace concurrency::detail {
// Inspects each component before canonicalization. Windows includes all reparse
// points; POSIX includes symlinks. Controlled-tree contract, not atomic openat.
core::Result<FileInfo> file_metadata(const std::filesystem::path& path, FileFailure& failure);
} // namespace concurrency::detail
