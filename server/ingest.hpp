#pragma once

#include "proto/frame_parser.hpp"
#include "proto/messages.hpp"
#include "server/connection.hpp"
#include "server/device_session.hpp"
#include "server/store.hpp"

#include <cstdint>
#include <optional>

namespace server {

// Wires one device's connection to protocol parsing and session policy:
// connection only knows about raw bytes, device_session only knows about
// HELLO/CONFIG/session policy -- ingest is the piece in between that turns
// "bytes arrived" into "a device just said HELLO, here is its CONFIG".
//
// One ingest exists per connected device -- created the moment a device's
// TCP connection is accepted, destroyed when it disconnects.
class ingest {
public:
    // session_id is the id this device's session will use once (if) it
    // sends HELLO -- assigned by the caller (whoever accepts connections),
    // the same contract device_session itself already uses, so uniqueness
    // across every currently-connected device is decided in exactly one
    // place, not duplicated here. data_store is where every SAMPLE this
    // device sends ends up, and is not owned by ingest -- one store is
    // shared by every device's ingest and every subscriber on the server.
    ingest(connection& conn, std::uint16_t session_id, store& data_store);

    // Drains conn's read buffer into the frame parser and dispatches every
    // frame it completes. Meant to be registered as conn's on_readable
    // callback (see connection.hpp).
    void on_readable();

    bool has_session() const { return session_.has_value(); }
    const device_session* session() const { return session_ ? &*session_ : nullptr; }

    // Sends a SLOW_DOWN frame lowering this device's rate to new_rate_hz,
    // and records that new rate on the session so a later call can halve it
    // again. A no-op before HELLO has been seen -- there is no session yet
    // to lower the rate of, and nothing on the wire that could tell a
    // not-yet-configured device to slow down. This is the piece that turns
    // "some subscriber's write buffer is congested" (server/main.cpp's
    // housekeeping sweep) into an actual byte reaching the MCU, closing the
    // backpressure loop end to end.
    void send_slow_down(std::uint8_t new_rate_hz);

private:
    void dispatch(const proto::ParsedFrame& frame);
    void handle_hello(const proto::HelloPayload& payload);
    void handle_heartbeat(const proto::HeartbeatPayload& payload);
    void handle_sample(const proto::SamplePayload& payload);
    void send_frame(proto::MessageType type, const std::uint8_t* payload, std::size_t payload_size);

    connection& connection_;
    std::uint16_t session_id_;
    store& store_;
    proto::frame_parser<> parser_;
    std::optional<device_session> session_;
};

}  // namespace server
