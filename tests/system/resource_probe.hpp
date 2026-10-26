#pragma once

#include <cstddef>
#include <optional>
#include <map>
#include <string>

namespace test {
struct ResourceSample {
    std::optional<std::size_t> handles;
    std::optional<std::size_t> threads;
    std::map<std::string, std::size_t> handle_types;
};
// Native test-only sampling. Never interpret an unavailable measurement as zero.
[[nodiscard]] ResourceSample resource_sample();
}
