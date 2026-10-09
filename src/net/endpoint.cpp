#include "net/endpoint.hpp"

#include <sstream>

namespace net {

std::string_view to_string(AddressFamily family) noexcept {
    switch (family) {
        case AddressFamily::ipv4:
            return "ipv4";
        case AddressFamily::ipv6:
            return "ipv6";
        case AddressFamily::unspecified:
        default:
            return "unspecified";
    }
}

Endpoint::Endpoint(AddressFamily family, std::string address, std::uint16_t port)
    : family_(family),
      address_(std::move(address)),
      port_(port) {}

core::Result<Endpoint> Endpoint::from_string(std::string_view ip_str, std::uint16_t port) {
    if (ip_str.empty()) {
        return core::make_error(
            core::ErrorCategory::invalid_argument,
            "Endpoint::from_string",
            0,
            "IP address string is empty"
        );
    }

    AddressFamily family = AddressFamily::ipv4;
    if (ip_str.find(':') != std::string_view::npos) {
        family = AddressFamily::ipv6;
    }

    return Endpoint(family, std::string(ip_str), port);
}

std::string Endpoint::to_string() const {
    std::ostringstream oss;
    if (family_ == AddressFamily::ipv6) {
        oss << "[" << address_ << "]:" << port_;
    } else {
        oss << address_ << ":" << port_;
    }
    return oss.str();
}

bool Endpoint::operator==(const Endpoint& other) const noexcept {
    return family_ == other.family_ &&
           port_ == other.port_ &&
           address_ == other.address_;
}

bool Endpoint::operator!=(const Endpoint& other) const noexcept {
    return !(*this == other);
}

std::string HostPort::to_string() const {
    std::ostringstream oss;
    oss << host << ":" << port;
    return oss.str();
}

} // namespace net
