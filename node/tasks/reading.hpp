#pragma once

#include <cstdint>

namespace tasks {

// What sensor_task produces: one raw sample, nothing else. Deliberately not
// proto::SamplePayload -- there is no session_id to stamp on anything yet
// (that only exists after the HELLO/CONFIG handshake, in a later
// sub-section). telemetry_task is what will turn a ProcessedReading into an
// actual wire-format proto::SamplePayload, once a session exists.
struct RawReading {
    std::int32_t value_milli;    // fake temperature, degrees C * 1000
    std::uint32_t timestamp_ms;  // uptime in ms at the moment of sampling
};

// What process_task produces: the same reading, smoothed, plus the one
// judgment call this stage adds -- is this cooling unit currently outside
// its safe range?
struct ProcessedReading {
    std::int32_t value_milli;
    std::uint32_t timestamp_ms;
    bool out_of_range;
};

}  // namespace tasks
