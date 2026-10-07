#pragma once

#include <system_error>

namespace net {

class Platform {
public:
    static void initialize(std::error_code& ec) noexcept;
    static void shutdown() noexcept;
};

}