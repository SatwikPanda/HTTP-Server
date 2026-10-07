#pragma once

#include "http/request.hpp"

#include <string_view>
#include <system_error>

namespace http {

class Parser {
public:

    static bool parse(
        std::string_view raw,
        Request& request,
        std::error_code& ec
    );
};

}