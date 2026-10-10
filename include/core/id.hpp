#pragma once

#include <cstdint>

namespace core {

struct ConnectionId {
    std::uint64_t value{};
    std::uint64_t generation{};
    bool operator==(const ConnectionId&) const = default;
};

} // namespace core
