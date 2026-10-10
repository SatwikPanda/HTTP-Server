#include "http/version.hpp"

namespace http {

std::string HttpVersion::to_string() const {
    return "HTTP/" + std::to_string(major) + "." + std::to_string(minor);
}

} // namespace http
