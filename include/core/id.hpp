#pragma once

#include <cstdint>

namespace core {

struct ConnectionId {
    std::uint64_t value{};
    std::uint64_t generation{};
    bool operator==(const ConnectionId&) const = default;
};

struct TransactionId {
    ConnectionId connection{};
    std::uint64_t sequence{};
    bool operator==(const TransactionId&) const = default;
};

enum class EndpointRole { downstream, upstream, file, resolver };
struct OperationId {
    TransactionId transaction{};
    EndpointRole endpoint{EndpointRole::downstream};
    std::uint64_t sequence{};
    bool operator==(const OperationId&) const = default;
};

} // namespace core
