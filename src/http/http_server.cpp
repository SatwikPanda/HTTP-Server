#include "http/response.hpp"

namespace http {

std::string Response::serialize() const {

    std::string result;

    result += "HTTP/1.1 ";
    result += std::to_string(status_code);
    result += " ";
    result += reason;
    result += "\r\n";

    for (const auto& [name, value] : headers) {

        result += name;
        result += ": ";
        result += value;
        result += "\r\n";
    }

    result += "\r\n";
    result += body;

    return result;
}

}