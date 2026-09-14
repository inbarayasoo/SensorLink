#include "server/subscriber.hpp"

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

subscriber::subscriber(connection& conn) : connection_(conn) {}

void subscriber::handle_command(const std::string& verb, const std::string& args) {
    if (verb == "SUBSCRIBE") {
        subscribed_metric_ = args;
    } else if (verb == "UNSUBSCRIBE") {
        subscribed_metric_.clear();
    } else if (verb == "STATS") {
        send_line("STATS metric=" + (subscribed_metric_.empty() ? "none" : subscribed_metric_) +
                   " updates_sent=" + std::to_string(updates_sent_));
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

void subscriber::publish(const std::string& metric, std::int32_t value_milli, std::uint32_t timestamp_ms) {
    if (metric != subscribed_metric_) {
        return;
    }
    send_line(metric + " " + std::to_string(timestamp_ms) + " " + std::to_string(value_milli));
    ++updates_sent_;
}

}  // namespace server
