#include "http/status.hpp"

namespace http {

std::string_view default_reason_phrase(StatusCode code) noexcept {
    return default_reason_phrase(static_cast<int>(code));
}

std::string_view default_reason_phrase(int code) noexcept {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";

        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";

        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";

        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "NotFound";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";

        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";

        default:  return "Unknown";
    }
}

bool is_valid_status_code(int code) noexcept {
    return code >= 100 && code <= 999;
}

bool is_valid_reason_phrase(std::string_view phrase) noexcept {
    for (unsigned char c : phrase) {
        // reason-phrase = *( HTAB / SP / VCHAR / obs-text )
        // HTAB = 0x09, SP = 0x20, VCHAR = 0x21-0x7E, obs-text = 0x80-0xFF
        if (c == 0x09 || (c >= 0x20 && c <= 0x7E) || c >= 0x80) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace http
