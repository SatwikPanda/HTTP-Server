#include "net/platform.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#pragma comment(lib, "Ws2_32.lib")

#else

#endif

namespace net {

#ifdef _WIN32

void Platform::initialize(std::error_code& ec) noexcept {

    WSADATA data{};

    if (WSAStartup(
        MAKEWORD(2, 2),
        &data
    ) != 0) {

        ec = std::error_code(
            WSAGetLastError(),
            std::system_category()
        );

        return;
    }

    ec.clear();
}

void Platform::shutdown() noexcept {
    WSACleanup();
}

#else

void Platform::initialize(std::error_code& ec) noexcept {
    ec.clear();
}

void Platform::shutdown() noexcept {}

#endif

}