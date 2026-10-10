#pragma once

#include "core/result.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tests {
// Opaque wrapper around a production response; all methods are declarations.
class Response {
public:
    Response();
    ~Response();
    Response(Response&&) noexcept;
    Response& operator=(Response&&) noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
Response fixed_response();
core::Result<std::string> serialize(const Response&);
std::size_t content_length(const Response&);
std::string header(const Response&, std::string_view);
std::vector<std::string> headers(const Response&, std::string_view);
void add_header(Response&, std::string_view, std::string_view);
std::string fixed_http_roundtrip(std::string_view, std::size_t max_write_chunk);
bool oversized_demo_header_is_rejected();
} // namespace tests
