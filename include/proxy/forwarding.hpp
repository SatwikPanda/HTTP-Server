#pragma once

#include "core/error.hpp"
#include "http/header.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/status.hpp"
#include "http/target.hpp"
#include "net/endpoint.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proxy {

// Forwarding header policy shared by the uncached proxy (B) and the caching
// proxy (K). Pure functions: no sockets, files, clocks or DNS.
struct ForwardingConfig {
    // Pseudonym appended to Via ("1.1 <pseudonym>") and searched for loops.
    // Must be a valid token of at most max_pseudonym_bytes.
    std::string via_pseudonym{"httpserver"};
    // Bound on the combined received Via value; longer chains are rejected.
    std::size_t max_via_bytes{1024};
};
inline constexpr std::size_t max_pseudonym_bytes = 64;

[[nodiscard]] core::Result<void> validate_forwarding_config(const ForwardingConfig& config);

// True for the fixed hop-by-hop set, independent of Connection nominations.
[[nodiscard]] bool is_hop_by_hop(std::string_view name) noexcept;

// Collects every Connection token (case-insensitive, lower-cased). Returns an
// error when a Connection member is not a valid token.
[[nodiscard]] core::Result<std::vector<std::string>> connection_tokens(const http::HeaderMap& headers);

// Copies end-to-end fields in order (preserving repeats such as Set-Cookie),
// dropping the fixed hop-by-hop set, every Connection-nominated field and the
// transfer framing fields Content-Length/Transfer-Encoding.
[[nodiscard]] core::Result<http::HeaderMap> end_to_end_fields(const http::HeaderMap& headers);

// True when any Via member names this pseudonym (received-by field match).
[[nodiscard]] bool via_contains(const http::HeaderMap& headers, std::string_view pseudonym) noexcept;

// Outbound origin request and the separate resolution target. `resolve.host`
// never contains IPv6 brackets; Host uses `target.authority.wire()`.
struct OutboundRequest {
    http::Target target;
    net::HostPort resolve;
    http::Request request;
};

// Either a prepared request or the client-facing rejection status.
struct Preparation {
    std::optional<OutboundRequest> outbound;
    http::StatusCode status{http::StatusCode::OK};
    core::Error error;
    [[nodiscard]] explicit operator bool() const noexcept { return outbound.has_value(); }
};

// Requires an absolute cleartext http URL. Regenerates Host from the selected
// authority, removes hop/proxy-credential fields, appends Via and asks the
// origin to close. Rejections: bad target/origin-form 400, Via loop 403,
// oversized/invalid Via or headers 400. The client request must already have
// passed the session's GET/HEAD/body/Expect/Upgrade restrictions.
[[nodiscard]] Preparation prepare_outbound(const http::Request& client, const ForwardingConfig& config);

// Builds the downstream head from an origin final head: same status/reason,
// end-to-end fields only, Via appended with the downstream protocol version.
// Rejects 101 and a response Via that already names this proxy.
[[nodiscard]] core::Result<http::Response> prepare_downstream_head(
    const http::Response& origin, const ForwardingConfig& config);

// Keeps only trailer fields that cannot affect routing/framing/hop state.
[[nodiscard]] http::HeaderMap forwardable_trailers(const http::HeaderMap& trailers, const http::HeaderMap& head);

} // namespace proxy
