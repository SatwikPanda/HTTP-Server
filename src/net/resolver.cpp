#include "net/resolver.hpp"
#include "detail/native.hpp"

#include <algorithm>
#include <memory>
#include <string>

namespace net {
core::Result<std::vector<Endpoint>> resolve(const HostPort& target) {
    if (target.host.empty() || target.host.find('\0') != std::string::npos || target.port == 0) {
        return core::make_error(core::ErrorCategory::invalid_argument, "resolve", 0,
                                "Host must be nonempty and port must be nonzero");
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* raw = nullptr;
    const auto service = std::to_string(target.port);
    const int code = ::getaddrinfo(target.host.c_str(), service.c_str(), &hints, &raw);
    if (code != 0) {
        return core::make_error(core::ErrorCategory::host_unreachable, "getaddrinfo", code,
                                "Could not resolve " + target.host);
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, &::freeaddrinfo);
    std::vector<Endpoint> endpoints;
    for (const addrinfo* entry = raw; entry != nullptr; entry = entry->ai_next) {
        const void* address = nullptr;
        AddressFamily family;
        if (entry->ai_family == AF_INET) {
            address = &reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr;
            family = AddressFamily::ipv4;
        } else if (entry->ai_family == AF_INET6) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(entry->ai_addr);
            // Endpoint currently represents numeric addresses without scope IDs.
            if (ipv6->sin6_scope_id != 0) continue;
            address = &ipv6->sin6_addr;
            family = AddressFamily::ipv6;
        } else continue;
        char text[INET6_ADDRSTRLEN]{};
        if (::inet_ntop(entry->ai_family, address, text, sizeof(text)) == nullptr) {
            const int error = detail::native_error();
            return core::make_error(core::ErrorCategory::io_error, "inet_ntop", error);
        }
        Endpoint endpoint(family, text, target.port);
        if (std::find(endpoints.begin(), endpoints.end(), endpoint) == endpoints.end()) {
            endpoints.push_back(std::move(endpoint));
        }
    }
    if (endpoints.empty()) {
        return core::make_error(core::ErrorCategory::host_unreachable, "resolve", 0,
                                "No supported IPv4 or IPv6 candidate addresses");
    }
    return endpoints;
}

core::Result<Socket> connect_candidates(std::span<const Endpoint> candidates, Deadline deadline) {
    if (std::chrono::steady_clock::now() >= deadline) {
        return core::make_error(core::ErrorCategory::timed_out, "connect_candidates");
    }
    if (candidates.empty()) {
        return core::make_error(core::ErrorCategory::invalid_argument, "connect_candidates", 0,
                                "No candidate addresses");
    }
    auto created_poller = Poller::create(deadline);
    if (!created_poller) return created_poller.error();
    auto poller = std::move(created_poller.value());
    core::Error last_error;
    std::uint64_t generation = 0;
    for (const auto& endpoint : candidates) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return core::make_error(core::ErrorCategory::timed_out, "connect_candidates");
        }
        auto created = create_tcp_socket(endpoint.family());
        if (!created) { last_error = created.error(); continue; }
        auto socket = std::move(created.value());
        auto begun = socket.begin_connect(endpoint);
        if (!begun) { last_error = begun.error(); continue; }
        if (begun.value() == ConnectState::connected) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return core::make_error(core::ErrorCategory::timed_out, "connect_candidates");
            }
            return socket;
        }
        auto watched = poller.watch(socket, Interest::write, {1, ++generation});
        if (!watched) return watched.error();
        auto token = watched.value();
        bool connected = false;
        for (;;) {
            auto ready = poller.wait(deadline);
            if (!ready) { last_error = ready.error(); break; }
            if (std::chrono::steady_clock::now() >= deadline) {
                last_error = core::make_error(core::ErrorCategory::timed_out, "connect_candidates");
                break;
            }
            if (ready.value().empty()) continue;
            auto completion = socket.finish_connect();
            if (completion) { connected = true; break; }
            if (completion.error().category == core::ErrorCategory::would_block) continue;
            last_error = completion.error();
            break;
        }
        auto unwatched = poller.unwatch(token);
        if (!unwatched) return unwatched.error();
        if (connected) return socket;
        socket.close();
        if (last_error.category == core::ErrorCategory::timed_out) return last_error;
    }
    return last_error;
}
} // namespace net
