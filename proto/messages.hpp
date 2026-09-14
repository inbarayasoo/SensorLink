#pragma once

#include <cstddef>
#include <cstdint>

// The single source of truth for the wire format. Both the firmware and the
// server include this file; a change here breaks both sides' tests at once.
//
// All multi-byte integer fields travel little-endian. Both build targets
// (x86-64 host, Cortex-M) are little-endian, so the packed structs below map
// directly onto the wire. The static_assert makes that assumption explicit.
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "SensorLink assumes a little-endian target"
#endif

namespace proto {

inline constexpr std::uint16_t kProtocolVersion = 0x0001;

// A frame on the wire, before COBS, is:
//   type (1) | length (1) | payload (length) | crc16 (2, big-endian / MSB first)
// The CRC covers type + length + payload. Payload integer fields are
// little-endian; the CRC itself is sent MSB first so a receiver's running CRC
// over the whole frame ends at zero.
inline constexpr std::size_t kFrameHeaderSize = 2;
inline constexpr std::size_t kFrameCrcSize = 2;
inline constexpr std::size_t kMaxPayloadSize = 255;

enum class MessageType : std::uint8_t {
    kHello = 1,      // device -> server: announce identity and capabilities
    kConfig = 2,     // server -> device: assign a session, set the sample rate
    kSample = 3,     // device -> server: one sensor reading
    kSlowDown = 4,   // server -> device: lower the sample rate (backpressure)
    kHeartbeat = 5,  // both directions: liveness ping
};

#pragma pack(push, 1)

struct FrameHeader {
    MessageType type;
    std::uint8_t length;  // payload byte count
};

struct HelloPayload {
    std::uint32_t device_id;
    std::uint16_t fw_version;
    std::uint8_t metric_count;
    std::uint8_t offered_rate_hz;
};

struct ConfigPayload {
    std::uint16_t session_id;
    std::uint8_t sample_rate_hz;
    std::uint8_t metric_mask;  // bit i set -> send metric i
};

struct SamplePayload {
    std::uint16_t session_id;
    std::uint32_t timestamp_ms;
    std::int32_t value_milli;  // reading * 1000, so no floats on the wire
    std::uint8_t metric_id;
};

struct SlowDownPayload {
    std::uint16_t session_id;
    std::uint8_t new_rate_hz;
};

struct HeartbeatPayload {
    std::uint16_t session_id;
    std::uint32_t uptime_ms;
};

#pragma pack(pop)

// Lock the layout: if any field type or order changes, the build fails here.
static_assert(sizeof(FrameHeader) == 2, "FrameHeader must be 2 bytes");
static_assert(sizeof(HelloPayload) == 8, "HelloPayload must be 8 bytes");
static_assert(sizeof(ConfigPayload) == 4, "ConfigPayload must be 4 bytes");
static_assert(sizeof(SamplePayload) == 11, "SamplePayload must be 11 bytes");
static_assert(sizeof(SlowDownPayload) == 3, "SlowDownPayload must be 3 bytes");
static_assert(sizeof(HeartbeatPayload) == 6, "HeartbeatPayload must be 6 bytes");

}  // namespace proto
