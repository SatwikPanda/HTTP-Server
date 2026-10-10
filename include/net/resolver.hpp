#pragma once

#include "net/poller.hpp"

#include <span>
#include <vector>

namespace net {
// Synchronous name lookup: call outside an event loop. DNS latency is not
// bounded by a socket deadline. Returned candidates retain getaddrinfo order.
[[nodiscard]] core::Result<std::vector<Endpoint>> resolve(const HostPort& target);

// Deadline-bounded, single-owner convenience adapter for already resolved
// candidates. Every failed attempt is unwatched and closed before a fresh socket
// is created. The returned connected socket remains nonblocking.
[[nodiscard]] core::Result<Socket> connect_candidates(std::span<const Endpoint>, Deadline);
} // namespace net
