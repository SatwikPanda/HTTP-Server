#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tests {
struct ParseResult { std::size_t consumed; bool error; };
class ParserHarness {
public:
    ParserHarness();
    ParseResult consume(std::string_view);
    bool complete() const;
    std::string target() const;
    std::vector<std::string> headers(std::string_view) const;
    void reset();
};
bool rejects_target_limit(std::size_t, std::string_view);
bool rejects_header_byte_limit(std::size_t);
bool rejects_header_count_limit(std::size_t);
bool rejects_connection_input_limit(std::size_t);
bool maps_syntax_to_status(int);
bool maps_version_to_status(int);
bool maps_header_limit_to_status(int);
} // namespace tests
