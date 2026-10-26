#include "proxy/forwarding.hpp"

#include "http/framing.hpp"
#include "http/serializer.hpp"

#include <algorithm>
#include <array>

namespace proxy {
namespace {
using http::header_name_equals;

core::Error invalid(std::string operation) {
    return core::make_error(core::ErrorCategory::invalid_argument, std::move(operation));
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}

std::string version_text(http::HttpVersion version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor);
}

std::size_t combined_size(const http::HeaderMap& headers, std::string_view name) {
    std::size_t total = 0;
    for (const auto value : headers.get_all(name)) total += value.size() + 2;
    return total;
}

Preparation reject(http::StatusCode status, core::Error error) {
    Preparation result;
    result.status = status;
    result.error = std::move(error);
    return result;
}
} // namespace

core::Result<void> validate_forwarding_config(const ForwardingConfig& config) {
    if (config.via_pseudonym.empty() || config.via_pseudonym.size() > max_pseudonym_bytes ||
        !http::is_valid_header_name(config.via_pseudonym) || config.max_via_bytes == 0 ||
        config.max_via_bytes > 64 * 1024) {
        return invalid("proxy forwarding config");
    }
    return {};
}

bool is_hop_by_hop(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 9> fields{
        "connection", "keep-alive", "proxy-connection", "proxy-authenticate",
        "proxy-authorization", "te", "trailer", "transfer-encoding", "upgrade"};
    return std::any_of(fields.begin(), fields.end(),
        [&](std::string_view field) { return header_name_equals(field, name); });
}

core::Result<std::vector<std::string>> connection_tokens(const http::HeaderMap& headers) {
    std::vector<std::string> tokens;
    for (auto value : headers.get_all("Connection")) {
        while (true) {
            const auto comma = value.find(',');
            const auto token = trim(value.substr(0, comma));
            // Empty list members are permitted by the list syntax; others must be tokens.
            if (!token.empty()) {
                if (!http::is_valid_header_name(token)) return invalid("Connection token");
                tokens.push_back(lower(token));
            }
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    return tokens;
}

core::Result<http::HeaderMap> end_to_end_fields(const http::HeaderMap& headers) {
    auto tokens = connection_tokens(headers);
    if (!tokens) return tokens.error();
    http::HeaderMap result;
    for (const auto& field : headers) {
        if (is_hop_by_hop(field.name) || header_name_equals(field.name, "Content-Length")) continue;
        const auto nominated = std::any_of(tokens.value().begin(), tokens.value().end(),
            [&](const std::string& token) { return header_name_equals(token, field.name); });
        if (!nominated) result.add(field.name, field.value);
    }
    return result;
}

bool via_contains(const http::HeaderMap& headers, std::string_view pseudonym) noexcept {
    for (auto value : headers.get_all("Via")) {
        while (true) {
            const auto comma = value.find(',');
            auto member = trim(value.substr(0, comma));
            // received-protocol RWS received-by [RWS comment]
            const auto space = member.find_first_of(" \t");
            if (space != std::string_view::npos) {
                member = trim(member.substr(space));
                const auto by = member.substr(0, member.find_first_of(" \t"));
                if (header_name_equals(by, pseudonym)) return true;
            }
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    return false;
}

Preparation prepare_outbound(const http::Request& client, const ForwardingConfig& config) {
    if (auto valid = validate_forwarding_config(config); !valid) {
        return reject(http::StatusCode::InternalServerError, valid.error());
    }
    // Proxy mode accepts only absolute-form; origin-form names no origin.
    if (client.target.empty() || client.target.front() == '/') {
        return reject(http::StatusCode::BadRequest, invalid("proxy absolute target"));
    }
    auto target = http::parse_target(client.target);
    if (!target || target.value().scheme != "http") {
        return reject(http::StatusCode::BadRequest, invalid("proxy target"));
    }
    if (combined_size(client.headers, "Via") > config.max_via_bytes) {
        return reject(http::StatusCode::BadRequest, invalid("proxy Via length"));
    }
    if (via_contains(client.headers, config.via_pseudonym)) {
        return reject(http::StatusCode::Forbidden, invalid("proxy Via loop"));
    }
    auto fields = end_to_end_fields(client.headers);
    if (!fields) return reject(http::StatusCode::BadRequest, fields.error());

    OutboundRequest outbound;
    outbound.target = std::move(target.value());
    outbound.resolve = {outbound.target.authority.host, outbound.target.authority.port};
    auto& request = outbound.request;
    request.method = client.method;
    request.raw_method = client.raw_method;
    request.target = outbound.target.path_query;
    request.version = http::HttpVersion::http_1_1();
    // The selected authority always wins over any client-supplied Host.
    request.headers.add("Host", outbound.target.authority.wire());
    for (const auto& field : fields.value()) {
        if (!header_name_equals(field.name, "Host")) request.headers.add(field.name, field.value);
    }
    request.headers.add("Via", version_text(client.version) + " " + config.via_pseudonym);
    // One fresh origin connection per transaction.
    request.headers.add("Connection", "close");
    if (auto wire = http::serialize_request_head(request); !wire) {
        return reject(http::StatusCode::BadRequest, wire.error());
    }
    Preparation result;
    result.outbound = std::move(outbound);
    return result;
}

core::Result<http::Response> prepare_downstream_head(const http::Response& origin, const ForwardingConfig& config) {
    if (origin.status_code < 200 || origin.status_code > 599) return invalid("proxy final status");
    if (combined_size(origin.headers, "Via") > config.max_via_bytes) return invalid("proxy response Via length");
    if (via_contains(origin.headers, config.via_pseudonym)) return invalid("proxy response Via loop");
    auto fields = end_to_end_fields(origin.headers);
    if (!fields) return fields.error();
    http::Response head(origin.status_code, origin.reason);
    head.headers = std::move(fields.value());
    head.headers.add("Via", version_text(origin.version) + " " + config.via_pseudonym);
    return head;
}

http::HeaderMap forwardable_trailers(const http::HeaderMap& trailers, const http::HeaderMap& head) {
    http::HeaderMap result;
    for (const auto& field : trailers) {
        if (is_hop_by_hop(field.name) || http::is_prohibited_trailer(field.name, &head) ||
            header_name_equals(field.name, "Via")) continue;
        result.add(field.name, field.value);
    }
    return result;
}

} // namespace proxy
