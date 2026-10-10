#pragma once

#include "http/header.hpp"
#include "http/method.hpp"
#include "http/version.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace http {

struct Request {
    Method method{Method::GET};
    std::string raw_method{"GET"};
    std::string target{"/"};
    HttpVersion version{1, 1};
    HeaderMap headers;
    std::string body;

    [[nodiscard]] std::optional<std::string_view> header(
        std::string_view name
    ) const noexcept {
        return headers.get(name);
    }
};

} // namespace http
