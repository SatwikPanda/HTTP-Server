#pragma once

#include "core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace server {

// invalid: malformed target syntax (400). forbidden: well-formed target that
// the document-root policy refuses (403). Missing files are an I/O result, not
// a policy result, and are decided later by the file port.
enum class TargetVerdict { ok, invalid, forbidden };

struct ResolvedTarget {
    TargetVerdict verdict{TargetVerdict::invalid};
    std::filesystem::path path;        // absolute, below the root; set only when ok
    std::vector<std::string> segments; // decoded, normalized, relative to the root
    bool trailing_slash{};             // the client named a directory, not a file
    const char* reason{""};            // short static diagnostic; never a local path
};

// Maps request targets to files under one canonical document root.
//
// Rules (all byte-exact, no locale-sensitive case folding or Unicode
// normalization; case sensitivity of the final lookup is the host file
// system's): the query is split off before decoding; %HH is decoded once per
// segment; '+' is a literal plus; decoded NUL, control bytes, backslash, '/' and
// invalid UTF-8 are invalid; ".." that would leave the root, colons (drive and
// alternate-stream forms), Windows reserved device names (ASCII case-folded,
// with extensions and trailing dots/spaces) and segments ending in '.' or ' '
// are forbidden. "/" maps to the configured index file.
class PathPolicy {
public:
    // Blocking: canonicalizes root with the nonthrowing filesystem overloads and
    // requires a directory. Call it at startup on a non-driver thread.
    [[nodiscard]] static core::Result<PathPolicy> create(const std::filesystem::path& root,
                                                         std::string index_file = "index.html");

    [[nodiscard]] ResolvedTarget resolve(std::string_view request_target) const;

    // Component-wise containment of an already canonical path, strictly below
    // the root. Never a string-prefix comparison.
    [[nodiscard]] bool contains(const std::filesystem::path& canonical) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    PathPolicy(std::filesystem::path root, std::string index_file);

    std::filesystem::path root_;
    std::vector<std::filesystem::path> root_components_;
    std::string index_file_;
};

// Exposed for tests: true for CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$,
// CONOUT$ (and superscript digit forms), ignoring ASCII case, an extension and
// trailing dots/spaces.
[[nodiscard]] bool is_reserved_device_name(std::string_view segment) noexcept;

} // namespace server
