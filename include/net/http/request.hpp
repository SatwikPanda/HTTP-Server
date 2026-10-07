#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace http {

struct Request {

    std::string method;

    std::string target;

    std::string version;

    std::unordered_map<
        std::string,
        std::string
    > headers;

    std::string body;

    [[nodiscard]]
    std::string_view header(
        std::string_view name
    ) const;
};

}