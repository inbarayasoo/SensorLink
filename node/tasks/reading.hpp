#pragma once

#include <cstdint>

namespace tasks {

// What sensor_task produces and telemetry_task sends on, unchanged in
// between. There is no smoothing/threshold stage on this side of the wire
// any more -- the server computes the moving average and decides whether a
// device is out of range itself, once the reading reaches it (see
// server/store.cpp::record()). This node only ever measures and forwards.
struct Reading {
    std::int32_t value_milli;    // fake temperature, degrees C * 1000
    std::uint32_t timestamp_ms;  // uptime in ms at the moment of sampling
};

}  // namespace tasks
