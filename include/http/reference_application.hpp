#pragma once
#include "http/application.hpp"
#include <filesystem>
#include <string>

namespace http {
// Fixed resources for exercising the foundation. These never resolve client
// paths/authorities: A/B supply routing, containment, forwarding and policy.
[[nodiscard]] std::shared_ptr<const TransactionFactory> make_file_reference_factory(std::filesystem::path path);
[[nodiscard]] std::shared_ptr<const TransactionFactory> make_origin_reference_factory(net::HostPort origin, std::string target = "/");
} // namespace http
