#include "server/subscriber.hpp"

#include "server/store.hpp"

#include <chrono>
#include <sstream>
#include <vector>

namespace server {

void subscriber::on_readable() {
    const std::vector<std::uint8_t>& bytes = connection_.read_buffer();

    std::size_t line_start = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '\n') {
            continue;
        }

        std::size_t line_end = i;
        if (line_end > line_start && bytes[line_end - 1] == '\r') {
            --line_end;
        }

        handle_line(std::string(bytes.begin() + static_cast<long>(line_start),
                                 bytes.begin() + static_cast<long>(line_end)));
        line_start = i + 1;
    }

    connection_.consume(line_start);
}

subscriber::subscriber(connection& conn, store& data_store) : connection_(conn), store_(data_store) {}

void subscriber::handle_command(const std::string& verb, const std::string& args) {
    if (verb == "SUBSCRIBE") {
        subscribed_metric_ = args;
    } else if (verb == "UNSUBSCRIBE") {
        subscribed_metric_.clear();
    } else if (verb == "STATS") {
        send_line("STATS metric=" + (subscribed_metric_.empty() ? "none" : subscribed_metric_) +
                   " updates_sent=" + std::to_string(updates_sent_));
    } else if (verb == "ACK") {
        // Same shape of check client/line_protocol.cpp's try_parse_sample()
        // already uses: the whole of args must be one decimal number and
        // nothing else. A malformed ACK is ignored the same way an unknown
        // verb is -- this file's existing stance is that nothing here needs
        // a protocol error reply.
        std::istringstream stream(args);
        std::uint16_t session_id;
        std::string trailing;
        if ((stream >> session_id) && !(stream >> trailing)) {
            const auto result = store_.ack(session_id, std::chrono::steady_clock::now());
            send_line("ACK_OK " + std::to_string(session_id) +
                       " excursion_ms=" + std::to_string(result.excursion_duration.count()) +
                       " excursion_active=" + (result.excursion_still_active ? "1" : "0") +
                       " offline_ms=" + std::to_string(result.offline_duration.count()));
        }
    }
    // Any other verb is silently ignored -- nothing in this project needs a
    // protocol error reply, so adding one now would be speculative scope.
}

void subscriber::handle_line(const std::string& line) {
    const std::size_t space = line.find(' ');
    const std::string verb = line.substr(0, space);
    const std::string args = (space == std::string::npos) ? std::string{} : line.substr(space + 1);
    handle_command(verb, args);
}

void subscriber::send_line(const std::string& line) {
    const std::string framed = line + "\n";
    connection_.enqueue_write(reinterpret_cast<const std::uint8_t*>(framed.data()), framed.size());
}

void subscriber::publish(const std::string& metric, std::uint16_t session_id,
                          std::int32_t value_milli, std::uint32_t timestamp_ms) {
    if (metric != subscribed_metric_) {
        return;
    }
    send_line(metric + " " + std::to_string(session_id) + " " + std::to_string(timestamp_ms) +
               " " + std::to_string(value_milli));
    ++updates_sent_;
}

void subscriber::publish_excursion_alert(std::uint16_t session_id, std::int32_t value_milli,
                                          std::uint32_t timestamp_ms) {
    send_line("ALERT EXCURSION " + std::to_string(session_id) + " " + std::to_string(timestamp_ms) +
               " " + std::to_string(value_milli));
}

void subscriber::publish_offline_alert(std::uint16_t session_id, std::uint32_t timestamp_ms) {
    send_line("ALERT OFFLINE " + std::to_string(session_id) + " " + std::to_string(timestamp_ms));
}

}  // namespace server
