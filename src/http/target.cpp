#include "http/target.hpp"
#include "http/header.hpp"
#include "net/endpoint.hpp"
#include <charconv>

namespace http {
namespace {
core::Error invalid() { return core::make_error(core::ErrorCategory::invalid_argument, "HTTP target"); }
bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
}
std::string Authority::wire() const {
    auto result = ipv6 ? "[" + host + "]" : host;
    if (port != 80) result += ":" + std::to_string(port);
    return result;
}
std::string Target::key() const { return scheme + "://" + authority.wire() + path_query; }
core::Result<Authority> parse_authority(std::string_view text) {
    if (text.empty() || text.size() > 8192) return invalid();
    Authority result;
    std::string_view host, port;
    bool has_port = false;
    if (text.front() == '[') {
        const auto end = text.find(']');
        if (end == std::string_view::npos) return invalid();
        host = text.substr(1, end - 1); result.ipv6 = true;
        auto numeric = net::Endpoint::from_string(host, 80);
        if (!numeric || numeric.value().family() != net::AddressFamily::ipv6) return invalid();
        if (end + 1 < text.size()) {
            if (text[end + 1] != ':') return invalid();
            has_port = true; port = text.substr(end + 2);
        }
    } else {
        const auto colon = text.find(':');
        host = text.substr(0, colon);
        if (colon != std::string_view::npos) { has_port = true; port = text.substr(colon + 1); }
        for (const unsigned char c : host) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '.' || c == '_')) return invalid();
        }
    }
    if (host.empty()) return invalid();
    result.host = host;
    for (auto& c : result.host) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (has_port) {
        unsigned number{};
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
        if (error != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535) return invalid();
        result.port = static_cast<std::uint16_t>(number);
    }
    return result;
}
core::Result<Target> parse_target(std::string_view text, std::string_view origin_authority) {
    if (text.empty() || text.size() > 8192) return invalid();
    std::string_view authority = origin_authority;
    std::string path;
    if (text.front() == '/') path = text;
    else {
        const auto scheme = text.find("://");
        if (scheme == std::string_view::npos || !header_name_equals(text.substr(0, scheme), "http")) return invalid();
        auto rest = text.substr(scheme + 3);
        const auto split = rest.find_first_of("/?");
        authority = rest.substr(0, split);
        path = split == std::string_view::npos ? "/" : std::string(rest.substr(split));
        if (path.front() == '?') path.insert(path.begin(), '/');
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto c = static_cast<unsigned char>(path[i]);
        if (c <= 32 || c >= 127 || c == '#' || c == '\\') return invalid();
        if (c == '%') {
            if (i + 2 >= path.size() || !hex(path[i + 1]) || !hex(path[i + 2])) return invalid();
            i += 2;
        }
    }
    auto parsed = parse_authority(authority);
    if (!parsed) return parsed.error();
    return Target{"http", std::move(parsed.value()), std::move(path)};
}
} // namespace http
