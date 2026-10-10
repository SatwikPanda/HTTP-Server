#include "http/method.hpp"

namespace http {

std::string_view to_string(Method method) noexcept {
    switch (method) {
        case Method::GET:     return "GET";
        case Method::HEAD:    return "HEAD";
        case Method::POST:    return "POST";
        case Method::PUT:     return "PUT";
        case Method::DELETE:  return "DELETE";
        case Method::CONNECT: return "CONNECT";
        case Method::OPTIONS: return "OPTIONS";
        case Method::TRACE:   return "TRACE";
        case Method::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<Method> method_from_string(std::string_view method_str) noexcept {
    if (method_str == "GET")     return Method::GET;
    if (method_str == "HEAD")    return Method::HEAD;
    if (method_str == "POST")    return Method::POST;
    if (method_str == "PUT")     return Method::PUT;
    if (method_str == "DELETE")  return Method::DELETE;
    if (method_str == "CONNECT") return Method::CONNECT;
    if (method_str == "OPTIONS") return Method::OPTIONS;
    if (method_str == "TRACE")   return Method::TRACE;
    return std::nullopt;
}

bool is_safe_method(Method method) noexcept {
    switch (method) {
        case Method::GET:
        case Method::HEAD:
        case Method::OPTIONS:
        case Method::TRACE:
            return true;
        default:
            return false;
    }
}

bool is_idempotent_method(Method method) noexcept {
    switch (method) {
        case Method::GET:
        case Method::HEAD:
        case Method::PUT:
        case Method::DELETE:
        case Method::OPTIONS:
        case Method::TRACE:
            return true;
        default:
            return false;
    }
}

} // namespace http
