#pragma once

#include <cstdint>

// Session state and the two outbound messages that establish and maintain
// it: HELLO (sent once, at startup) and HEARTBEAT (sent periodically,
// forever after). This is deliberately separate from command_task.cpp:
// this file is the *policy* ("what does a session mean, what goes in a
// HELLO"), command_task.cpp is the *mechanism* ("read bytes, find frames,
// call into this file at the right moments"). The same separation already
// exists between process_task (policy: what counts as out-of-range) and
// sensor_task (mechanism: how a reading gets produced).
namespace tasks {

struct Pipeline;

// Everything about the device's current conversation with a server. Before
// the first CONFIG arrives, connected is false and session_id is
// meaningless -- exactly like a walk-in cooling unit that has power and is
// sending HELLO on a loop, but has not yet been acknowledged by the control
// room.
struct SessionState {
    std::uint16_t session_id = 0;
    bool connected = false;
};

// Encodes and sends one HELLO frame over pipeline->uart. Called once, from
// command_task, before it starts listening for a reply.
void SendHello(Pipeline& pipeline);

// Encodes and sends one HEARTBEAT frame over pipeline->uart, stamped with
// the current session_id (0 if not yet connected -- a real server would
// still see these and could plausibly treat repeated pre-connection
// heartbeats as "device present, still waiting to be configured").
void SendHeartbeat(Pipeline& pipeline);

}  // namespace tasks
