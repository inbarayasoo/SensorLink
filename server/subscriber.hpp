#pragma once

#include "server/connection.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace server {

// The line-based text protocol a dashboard or alerting client speaks:
//   SUBSCRIBE <metric>
//   UNSUBSCRIBE
//   STATS
// This is deliberately the plain-text sibling of ingest's binary COBS
// framing: a device is a weak, constrained MCU that needs the smallest
// possible wire format, but a subscriber is an ordinary program on an
// ordinary machine -- readable text costs it nothing, and it is far easier
// to poke at by hand (telnet, nc) while developing.
//
// One subscriber exists per connected dashboard/alerting client -- one per
// screen in the cold-chain QA control room.
class subscriber {
public:
    explicit subscriber(connection& conn);

    // Drains newly-arrived bytes, splits them into complete lines, and
    // executes each one as a command. Meant to be registered as conn's
    // on_readable callback, the same way ingest is.
    void on_readable();

    // If this subscriber is currently subscribed to `metric`, encodes and
    // enqueues one update line for it; otherwise does nothing at all -- an
    // unsubscribed metric costs this subscriber exactly zero bytes. Called
    // by whatever feeds it live samples (store, a later sub-section).
    void publish(const std::string& metric, std::int32_t value_milli, std::uint32_t timestamp_ms);

    bool subscribed() const { return !subscribed_metric_.empty(); }
    const std::string& subscribed_metric() const { return subscribed_metric_; }
    std::size_t updates_sent() const { return updates_sent_; }

private:
    void handle_line(const std::string& line);
    void handle_command(const std::string& verb, const std::string& args);
    void send_line(const std::string& line);

    connection& connection_;
    std::string subscribed_metric_;
    std::size_t updates_sent_ = 0;
};

}  // namespace server
