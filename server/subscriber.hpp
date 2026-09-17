#pragma once

#include "server/connection.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace server {

class store;

// The line-based text protocol a dashboard or alerting client speaks:
//   SUBSCRIBE <metric>
//   UNSUBSCRIBE
//   STATS
//   ACK <session_id>
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
    // data_store is where ACK gets forwarded -- the one direction this
    // class talks back to store, rather than only being talked to by it.
    // Not owned here, same as every other reference to the shared store.
    subscriber(connection& conn, store& data_store);

    // Drains newly-arrived bytes, splits them into complete lines, and
    // executes each one as a command. Meant to be registered as conn's
    // on_readable callback, the same way ingest is.
    void on_readable();

    // If this subscriber is currently subscribed to `metric`, encodes and
    // enqueues one update line for it; otherwise does nothing at all -- an
    // unsubscribed metric costs this subscriber exactly zero bytes. Called
    // by store for every recorded sample.
    void publish(const std::string& metric, std::uint16_t session_id,
                 std::int32_t value_milli, std::uint32_t timestamp_ms);

    // Sent unconditionally, ignoring subscribed_metric_ -- an alert is not
    // a per-metric live feed update, it is a "look at this device" signal
    // every connected client should see. Called by store when a new alert
    // latches, and again (via add_subscriber's replay) for one already
    // active when this subscriber first connects.
    void publish_excursion_alert(std::uint16_t session_id, std::int32_t value_milli,
                                  std::uint32_t timestamp_ms);
    void publish_offline_alert(std::uint16_t session_id, std::uint32_t timestamp_ms);

    bool subscribed() const { return !subscribed_metric_.empty(); }
    const std::string& subscribed_metric() const { return subscribed_metric_; }
    std::size_t updates_sent() const { return updates_sent_; }

private:
    void handle_line(const std::string& line);
    void handle_command(const std::string& verb, const std::string& args);
    void send_line(const std::string& line);

    connection& connection_;
    store& store_;
    std::string subscribed_metric_;
    std::size_t updates_sent_ = 0;
};

}  // namespace server
