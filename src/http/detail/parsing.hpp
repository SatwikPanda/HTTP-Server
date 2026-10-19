#pragma once

#include "http/framing.hpp"
#include "http/header.hpp"
#include "http/request_parser.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace http::detail {

inline std::string_view trim_ows(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

// A nonempty, CRLF-stripped field line. Trailers remain separate from headers.
inline ParseError parse_field_line(std::string_view line, HeaderMap& fields,
                                   std::size_t limit, const HeaderMap* head = nullptr) {
    if (line.front() == ' ' || line.front() == '\t') return ParseError::obsolete_line_folding;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return ParseError::malformed_header;
    const auto name = line.substr(0, colon);
    const auto value = trim_ows(line.substr(colon + 1));
    if (!is_valid_header_name(name) || !is_valid_header_value(value)) return ParseError::malformed_header;
    if (head && is_prohibited_trailer(name, head)) return ParseError::prohibited_trailer;
    if (fields.size() >= limit) return head ? ParseError::too_many_trailers : ParseError::too_many_headers;
    fields.add(std::string(name), std::string(value));
    return ParseError::none;
}

} // namespace http::detail
