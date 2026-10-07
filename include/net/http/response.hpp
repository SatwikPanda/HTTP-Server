#pragma once

#include <string>
#include <unordered_map>

namespace http {

struct Response {

    int status_code{200};

    std::string reason{"OK"};

    std::unordered_map<
        std::string,
        std::string
    > headers;

    std::string body;

    [[nodiscard]]
    std::string serialize() const;
};

}