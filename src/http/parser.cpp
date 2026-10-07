#include "http/parser.hpp"

#include <algorithm>
#include <sstream>

namespace {

std::string trim(std::string value) {

    auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) {
        return {};
    }

    auto last = value.find_last_not_of(" \t\r\n");

    return value.substr(
        first,
        last - first + 1
    );
}

}

namespace http {

bool Parser::parse(
    std::string_view raw,
    Request& request,
    std::error_code& ec
) {

    ec.clear();

    auto header_end = raw.find("\r\n\r\n");

    if (header_end == std::string_view::npos) {

        ec = std::make_error_code(
            std::errc::invalid_argument
        );

        return false;
    }

    std::string header_part(
        raw.substr(0, header_end)
    );

    std::istringstream stream(header_part);

    std::string request_line;

    if (!std::getline(stream, request_line)) {
        return false;
    }

    if (!request_line.empty() &&
        request_line.back() == '\r') {

        request_line.pop_back();
    }

    std::istringstream request_stream(
        request_line
    );

    if (!(request_stream >>
          request.method >>
          request.target >>
          request.version)) {

        ec = std::make_error_code(
            std::errc::invalid_argument
        );

        return false;
    }

    std::string line;

    while (std::getline(stream, line)) {

        if (!line.empty() &&
            line.back() == '\r') {

            line.pop_back();
        }

        auto separator = line.find(':');

        if (separator == std::string::npos) {
            continue;
        }

        std::string name =
            line.substr(0, separator);

        std::string value =
            line.substr(separator + 1);

        request.headers[
            std::move(name)
        ] = trim(std::move(value));
    }

    request.body = std::string(
        raw.substr(header_end + 4)
    );

    return true;
}

}