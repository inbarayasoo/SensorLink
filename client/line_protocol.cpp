#include "client/line_protocol.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace client {

namespace {

bool try_parse_sample(const std::string& line, std::string& metric,
                       std::uint32_t& timestamp_ms, std::int32_t& value_milli) {
    std::istringstream stream(line);
    std::string trailing;
    if (!(stream >> metric >> timestamp_ms >> value_milli)) {
        return false;
    }
    return !(stream >> trailing);
}

}  // namespace

void handle_incoming(std::vector<std::uint8_t>& buffer, std::size_t& valid_count, std::size_t& malformed_count) {
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] != '\n') {
            continue;
        }

        std::size_t line_end = i;
        if (line_end > line_start && buffer[line_end - 1] == '\r') {
            --line_end;
        }

        const std::string line(buffer.begin() + static_cast<long>(line_start),
                                buffer.begin() + static_cast<long>(line_end));

        std::string metric;
        std::uint32_t timestamp_ms = 0;
        std::int32_t value_milli = 0;
        if (try_parse_sample(line, metric, timestamp_ms, value_milli)) {
            std::cout << metric << ": " << value_milli / 1000.0 << " (t=" << timestamp_ms << "ms)" << std::endl;
            ++valid_count;
        } else {
            std::cout << "unrecognized line: " << line << std::endl;
            ++malformed_count;
        }

        line_start = i + 1;
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(line_start));
}

}  // namespace client