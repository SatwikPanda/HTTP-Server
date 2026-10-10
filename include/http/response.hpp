#pragma once

#include "http/header.hpp"
#include "http/status.hpp"
#include "http/version.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace http {

struct Response {
    HttpVersion version{1, 1};
    StatusCode status{StatusCode::OK};
    int status_code{200};
    std::string reason{"OK"};
    HeaderMap headers;
    std::string body;

    Response() = default;

    explicit Response(StatusCode code)
        : status(code),
          status_code(static_cast<int>(code)),
          reason(default_reason_phrase(code)) {}

    Response(int code, std::string custom_reason)
        : status(static_cast<StatusCode>(code)),
          status_code(code),
          reason(std::move(custom_reason)) {}

    void set_status(StatusCode code) {
        status = code;
        status_code = static_cast<int>(code);
        reason = default_reason_phrase(code);
    }

    void set_status(int code, std::string custom_reason = {}) {
        status = static_cast<StatusCode>(code);
        status_code = code;
        if (!custom_reason.empty()) {
            reason = std::move(custom_reason);
        } else {
            reason = default_reason_phrase(code);
        }
    }

    void set_body(
        std::string new_body,
        std::string_view content_type = "text/plain; charset=utf-8"
    ) {
        body = std::move(new_body);
        headers.set("Content-Type", std::string(content_type));
        headers.set("Content-Length", std::to_string(body.size()));
    }

    [[nodiscard]] std::optional<std::string_view> header(
        std::string_view name
    ) const noexcept {
        return headers.get(name);
    }
};

} // namespace http
