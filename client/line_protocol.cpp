#include "client/line_protocol.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace client {

namespace {

bool try_parse_sample(const std::string& line, std::string& metric, std::uint16_t& session_id,
                       std::uint32_t& timestamp_ms, std::int32_t& value_milli) {
    std::istringstream stream(line);
    std::string trailing;
    if (!(stream >> metric >> session_id >> timestamp_ms >> value_milli)) {
        return false;
    }
    return !(stream >> trailing);
}

// "ALERT EXCURSION <session_id> <timestamp_ms> <value_milli>" or
// "ALERT OFFLINE <session_id> <timestamp_ms>" -- the two shapes
// subscriber.cpp's publish_excursion_alert()/publish_offline_alert() send.
// kind and value_milli come back exactly as parsed; value_milli is
// meaningless (left at 0) for OFFLINE, which carries none.
bool try_parse_alert(const std::string& line, std::string& kind, std::uint16_t& session_id,
                      std::uint32_t& timestamp_ms, std::int32_t& value_milli) {
    std::istringstream stream(line);
    std::string verb;
    if (!(stream >> verb) || verb != "ALERT") {
        return false;
    }
    if (!(stream >> kind >> session_id >> timestamp_ms)) {
        return false;
    }

    std::string trailing;
    if (kind == "EXCURSION") {
        return (stream >> value_milli) && !(stream >> trailing);
    }
    if (kind == "OFFLINE") {
        value_milli = 0;
        return !(stream >> trailing);
    }
    return false;
}

}  // namespace

void handle_incoming(std::vector<std::uint8_t>& buffer, std::size_t& valid_count,
                      std::size_t& alert_count, std::size_t& malformed_count) {
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
        std::uint16_t session_id = 0;
        std::uint32_t timestamp_ms = 0;
        std::int32_t value_milli = 0;
        std::string kind;

        if (try_parse_sample(line, metric, session_id, timestamp_ms, value_milli)) {
            std::cout << metric << ": " << value_milli / 1000.0 << " (session=" << session_id
                      << ", t=" << timestamp_ms << "ms)" << std::endl;
            ++valid_count;
        } else if (try_parse_alert(line, kind, session_id, timestamp_ms, value_milli)) {
            std::cout << "ALERT " << kind << " session=" << session_id << " t=" << timestamp_ms << "ms";
            if (kind == "EXCURSION") {
                std::cout << " value=" << value_milli / 1000.0;
            }
            std::cout << std::endl;
            ++alert_count;
        } else {
            std::cout << "unrecognized line: " << line << std::endl;
            ++malformed_count;
        }

        line_start = i + 1;
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(line_start));
}

}  // namespace client
