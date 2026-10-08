#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace net {

enum class AddressFamily {
    unspecified = 0,
    ipv4,
    ipv6
};

[[nodiscard]] std::string_view to_string(AddressFamily family) noexcept;

class Endpoint {
public:
    Endpoint() = default;
    Endpoint(AddressFamily family, std::string address, std::uint16_t port);

    [[nodiscard]] static Endpoint ipv4_any(std::uint16_t port) {
        return Endpoint(AddressFamily::ipv4, "0.0.0.0", port);
    }

    [[nodiscard]] static Endpoint ipv4_loopback(std::uint16_t port) {
        return Endpoint(AddressFamily::ipv4, "127.0.0.1", port);
    }

    [[nodiscard]] static Endpoint ipv6_any(std::uint16_t port) {
        return Endpoint(AddressFamily::ipv6, "::", port);
    }

    [[nodiscard]] static Endpoint ipv6_loopback(std::uint16_t port) {
        return Endpoint(AddressFamily::ipv6, "::1", port);
    }

    [[nodiscard]] static core::Result<Endpoint> from_string(std::string_view ip_str, std::uint16_t port);

    [[nodiscard]] AddressFamily family() const noexcept { return family_; }
    [[nodiscard]] const std::string& address() const noexcept { return address_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::string to_string() const;

    bool operator==(const Endpoint& other) const noexcept;
    bool operator!=(const Endpoint& other) const noexcept;

private:
    AddressFamily family_{AddressFamily::ipv4};
    std::string address_{"127.0.0.1"};
    std::uint16_t port_{0};
};

struct HostPort {
    std::string host;
    std::uint16_t port{0};

    [[nodiscard]] std::string to_string() const;
};

} // namespace net
