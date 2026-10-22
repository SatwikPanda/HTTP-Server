#include "server/path_policy.hpp"

#include "http/target.hpp"

#include <array>
#include <cstdint>

namespace server {
namespace {
constexpr std::size_t max_segment_bytes = 255;

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char ascii_upper(char c) noexcept { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (lead < 0x80) { ++i; continue; }
        if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; code = lead & 0x1Fu; }
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; code = lead & 0x0Fu; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; code = lead & 0x07u; }
        else return false;
        if (i + extra >= text.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0u) != 0x80u) return false;
            code = (code << 6) | (next & 0x3Fu);
        }
        // Reject overlong forms, UTF-16 surrogates and values past U+10FFFF.
        if ((extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
            (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return false;
        i += extra + 1;
    }
    return true;
}

// Decodes one raw segment exactly once. Returns nullptr on success, otherwise a
// static reason. Encoded separators are rejected so a decoded '/' can never
// bypass the per-segment checks that follow.
const char* decode_segment(std::string_view raw, std::string& decoded) {
    decoded.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char byte = raw[i];
        if (byte == '%') {
            if (i + 2 >= raw.size()) return "incomplete percent escape";
            const int high = hex_value(raw[i + 1]);
            const int low = hex_value(raw[i + 2]);
            if (high < 0 || low < 0) return "malformed percent escape";
            byte = static_cast<char>(high * 16 + low);
            i += 2;
        }
        const auto value = static_cast<unsigned char>(byte);
        if (value == 0) return "NUL byte";
        if (value < 0x20 || value == 0x7F) return "control byte";
        if (byte == '\\') return "backslash";
        if (byte == '/') return "encoded separator";
        decoded.push_back(byte);
    }
    if (decoded.size() > max_segment_bytes) return "segment too long";
    if (!valid_utf8(decoded)) return "invalid UTF-8";
    return nullptr;
}

// Segment forms that name something other than a plain file on at least one
// supported platform. The same tree must behave the same everywhere.
const char* forbidden_reason(std::string_view segment) noexcept {
    if (segment.find(':') != std::string_view::npos) return "colon (drive or stream form)";
    if (segment.find_first_of("<>\"|*?") != std::string_view::npos) return "reserved character";
    if (segment.back() == '.' || segment.back() == ' ') return "trailing dot or space";
    if (is_reserved_device_name(segment)) return "reserved device name";
    return nullptr;
}

std::filesystem::path from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
} // namespace

bool is_reserved_device_name(std::string_view segment) noexcept {
    auto stem = segment.substr(0, segment.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
    std::array<char, 8> upper{};
    if (stem.empty() || stem.size() >= upper.size()) return false;
    for (std::size_t i = 0; i < stem.size(); ++i) upper[i] = ascii_upper(stem[i]);
    const std::string_view name(upper.data(), stem.size());
    if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" ||
        name == "CONIN$" || name == "CONOUT$") return true;
    if (name.size() == 4 && (name.starts_with("COM") || name.starts_with("LPT")) &&
        name[3] >= '0' && name[3] <= '9') return true;
    // COM and LPT followed by superscript one, two or three (UTF-8 C2 B9/B2/B3).
    if (name.size() == 5 && (name.starts_with("COM") || name.starts_with("LPT")) &&
        static_cast<unsigned char>(name[3]) == 0xC2) {
        const auto tail = static_cast<unsigned char>(name[4]);
        return tail == 0xB9 || tail == 0xB2 || tail == 0xB3;
    }
    return false;
}

PathPolicy::PathPolicy(std::filesystem::path root, std::string index_file)
    : root_(std::move(root)), index_file_(std::move(index_file)) {
    for (const auto& component : root_) root_components_.push_back(component);
}

core::Result<PathPolicy> PathPolicy::create(const std::filesystem::path& root, std::string index_file) {
    auto invalid = [](const char* message) {
        return core::make_error(core::ErrorCategory::invalid_argument, "document root", 0, message);
    };
    if (root.empty()) return invalid("document root is empty");
    if (index_file.empty() || index_file == "." || index_file == ".." || index_file.size() > max_segment_bytes ||
        index_file.find_first_of("/\\") != std::string::npos || !valid_utf8(index_file) ||
        forbidden_reason(index_file) != nullptr) return invalid("index file name is not a plain file name");
    for (const char c : index_file) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return invalid("index file name has control bytes");
    std::error_code error;
    auto canonical = std::filesystem::canonical(root, error);
    if (error) return core::make_error(core::ErrorCategory::io_error, "document root canonical path", error.value(), "document root cannot be resolved");
    const auto status = std::filesystem::status(canonical, error);
    if (error) return core::make_error(core::ErrorCategory::io_error, "document root status", error.value(), "document root cannot be inspected");
    if (!std::filesystem::is_directory(status)) return invalid("document root is not a directory");
    return PathPolicy(std::move(canonical), std::move(index_file));
}

bool PathPolicy::contains(const std::filesystem::path& canonical) const {
    auto it = canonical.begin();
    for (const auto& component : root_components_) {
        if (it == canonical.end() || *it != component) return false;
        ++it;
    }
    return it != canonical.end();
}

ResolvedTarget PathPolicy::resolve(std::string_view request_target) const {
    ResolvedTarget out;
    auto reject = [&](TargetVerdict verdict, const char* reason) {
        out.verdict = verdict; out.reason = reason;
        out.path.clear(); out.segments.clear(); out.trailing_slash = false;
        return out;
    };
    if (request_target.empty()) return reject(TargetVerdict::invalid, "empty target");

    // Origin-form is handled here; absolute-form goes through the shared parser
    // and only its path/query is used. A whole URL never becomes a file path.
    std::string absolute_path_query;
    std::string_view path_query = request_target;
    if (request_target.front() != '/') {
        auto parsed = http::parse_target(request_target);
        if (!parsed) return reject(TargetVerdict::invalid, "unsupported request target form");
        absolute_path_query = std::move(parsed.value().path_query);
        path_query = absolute_path_query;
    }
    for (const char c : path_query) {
        const auto value = static_cast<unsigned char>(c);
        if (value <= 0x20 || value >= 0x7F) return reject(TargetVerdict::invalid, "raw whitespace, control or non-ASCII byte");
        if (c == '#') return reject(TargetVerdict::invalid, "fragment in request target");
        if (c == '\\') return reject(TargetVerdict::invalid, "backslash");
    }
    if (path_query.empty() || path_query.front() != '/') return reject(TargetVerdict::invalid, "target has no path");

    // The query is split off before any decoding and never affects lookup.
    const auto question = path_query.find('?');
    const auto path = path_query.substr(1, question == std::string_view::npos ? std::string_view::npos : question - 1);

    std::vector<std::string> stack;
    bool trailing_slash = false;
    std::string decoded;
    for (std::size_t position = 0; !path.empty();) {
        const auto slash = path.find('/', position);
        const bool last = slash == std::string_view::npos;
        const auto raw = path.substr(position, last ? std::string_view::npos : slash - position);
        if (raw.empty()) {
            if (!last) return reject(TargetVerdict::invalid, "empty path segment");
            trailing_slash = true;
            break;
        }
        if (const char* reason = decode_segment(raw, decoded)) return reject(TargetVerdict::invalid, reason);
        if (decoded == "." || decoded == "..") {
            if (decoded == ".." ) {
                if (stack.empty()) return reject(TargetVerdict::forbidden, "traversal outside the document root");
                stack.pop_back();
            }
            if (last) trailing_slash = true; // names a directory
        } else {
            if (const char* reason = forbidden_reason(decoded)) return reject(TargetVerdict::forbidden, reason);
            stack.push_back(decoded);
        }
        if (last) break;
        position = slash + 1;
    }
    if (stack.empty()) { stack.push_back(index_file_); trailing_slash = false; }

    out.path = root_;
    for (const auto& segment : stack) out.path /= from_utf8(segment);
    out.segments = std::move(stack);
    out.trailing_slash = trailing_slash;
    out.verdict = TargetVerdict::ok;
    out.reason = "";
    return out;
}
} // namespace server
