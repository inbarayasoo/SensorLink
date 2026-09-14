#pragma once

#include "proto/messages.hpp"

#include <chrono>
#include <cstdint>

namespace server {

// Session policy for one connected device: what a HELLO gets answered with,
// and whether the device has gone quiet. Deliberately free of sockets,
// buffers, or epoll -- the same "policy vs mechanism" split node/session.hpp
// uses on the firmware side. ingest (a later sub-section) is the mechanism:
// it owns the connection and the frame_parser, and calls into a
// device_session whenever a frame decodes.
//
// One device_session exists per device currently talking to the server --
// one per walk-in freezer, one per delivery truck, one per pharmacy fridge.
class device_session {
public:
    // session_id is assigned by the caller (ingest, once HELLO arrives) --
    // uniqueness across every currently-connected device is that caller's
    // responsibility, not this class's. now is the construction time, used
    // as the initial "last heard from this device" mark so a session is
    // never considered timed out before it has received a single frame.
    device_session(std::uint16_t session_id, std::chrono::steady_clock::time_point now);

    // Decides the CONFIG reply to a device's HELLO: caps the offered sample
    // rate at a server-side maximum, and sets the metric_mask bit for every
    // metric the device offered.
    proto::ConfigPayload handle_hello(const proto::HelloPayload& hello);

    // Marks the device as heard-from right now. Called for every frame that
    // arrives from it -- HEARTBEAT included, but not only HEARTBEAT, since
    // a SAMPLE arriving is just as good a sign the device is alive.
    void note_activity(std::chrono::steady_clock::time_point now);

    // True once more than `timeout` has passed since the last note_activity.
    bool timed_out(std::chrono::steady_clock::time_point now,
                   std::chrono::milliseconds timeout) const;

    // Builds a SLOW_DOWN payload lowering the device's rate. Called by
    // whichever component notices a subscriber falling behind -- the piece
    // that closes the backpressure loop end to end, from a slow dashboard
    // all the way back to the sampling rate on the MCU.
    proto::SlowDownPayload slow_down(std::uint8_t new_rate_hz);

    std::uint16_t session_id() const { return session_id_; }
    std::uint8_t current_rate_hz() const { return current_rate_hz_; }
    bool connected() const { return connected_; }

private:
    std::uint16_t session_id_;
    bool connected_ = false;
    std::uint8_t current_rate_hz_ = 0;
    std::chrono::steady_clock::time_point last_activity_;
};

}  // namespace server
