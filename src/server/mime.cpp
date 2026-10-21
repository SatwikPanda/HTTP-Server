#include "server/mime.hpp"

namespace server {
namespace {
struct Entry { std::string_view extension; std::string_view type; };
constexpr Entry table[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"xml", "application/xml"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"ico", "image/x-icon"},
    {"bmp", "image/bmp"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"wasm", "application/wasm"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"mp3", "audio/mpeg"},
    {"wav", "audio/wav"},
    {"ogg", "audio/ogg"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mov", "video/quicktime"},
    {"bin", "application/octet-stream"},
};

bool equals_ascii_folded(std::string_view text, std::string_view lower_name) noexcept {
    if (text.size() != lower_name.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lower_name[i]) return false;
    }
    return true;
}
} // namespace

std::string_view mime_type_for(std::string_view file_name) noexcept {
    const auto dot = file_name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 == file_name.size()) return binary_mime_type;
    const auto extension = file_name.substr(dot + 1);
    for (const auto& entry : table) {
        if (equals_ascii_folded(extension, entry.extension)) return entry.type;
    }
    return binary_mime_type;
}
} // namespace server
