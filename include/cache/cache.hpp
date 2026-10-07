#pragma once

#include "http/response.hpp"

#include <optional>
#include <string>

namespace cache {

class Cache {
public:

    virtual ~Cache() = default;

    virtual std::optional<http::Response>
    get(const std::string& key) = 0;

    virtual void put(
        const std::string& key,
        const http::Response& response
    ) = 0;

    virtual void remove(
        const std::string& key
    ) = 0;

    virtual void clear() = 0;
};

}