#pragma once

#include <string_view>

namespace server {

// Content-Type for a file name from an explicit extension table. The extension
// is the text after the last '.', matched with ASCII-only case folding (no
// locale dependence). Unknown or missing extensions get application/octet-stream.
[[nodiscard]] std::string_view mime_type_for(std::string_view file_name) noexcept;

inline constexpr std::string_view binary_mime_type = "application/octet-stream";

} // namespace server
