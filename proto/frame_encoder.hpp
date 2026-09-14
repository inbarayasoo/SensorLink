#pragma once

#include <cstddef>
#include <cstdint>

#include "proto/cobs.hpp"
#include "proto/crc16.hpp"
#include "proto/messages.hpp"

// Turns (message type, payload bytes) into a complete frame ready for the wire:
//
//   1. lay out   type | length | payload | crc16 (big-endian, MSB first)
//   2. COBS-encode the whole blob so it contains no 0x00
//   3. append a single 0x00 delimiter
//
// The CRC is sent most-significant byte first on purpose: a receiver can run its
// running CRC over the whole decoded blob and simply check that it reaches 0.
//
// Allocation-free: the caller supplies the output buffer. The mirror image of
// this lives in the frame parser on the receiving side.
namespace proto {

enum class EncodeStatus : std::uint8_t {
    kOk,
    kPayloadTooLarge,   // payload_size exceeds kMaxPayloadSize
    kOutputOverflow,    // caller's output buffer is too small
};

struct EncodeResult {
    EncodeStatus status;
    std::size_t size;  // frame length written to out; valid when status == kOk
};

// Blob size before COBS, worst case: header + max payload + crc.
inline constexpr std::size_t kMaxFrameBeforeCobs =
    kFrameHeaderSize + kMaxPayloadSize + kFrameCrcSize;

// Upper bound on the encoded frame size for a given payload length.
constexpr std::size_t max_encoded_frame_size(std::size_t payload_size) noexcept {
    return cobs_encoded_max_size(kFrameHeaderSize + payload_size + kFrameCrcSize) + 1;
}

constexpr EncodeResult encode_frame(MessageType type,
                                    const std::uint8_t* payload, std::size_t payload_size,
                                    std::uint8_t* out, std::size_t out_capacity) noexcept {
    if (payload_size > kMaxPayloadSize) {
        return {EncodeStatus::kPayloadTooLarge, 0};
    }
    if (out_capacity == 0) {
        return {EncodeStatus::kOutputOverflow, 0};
    }

    std::uint8_t blob[kMaxFrameBeforeCobs] = {};
    blob[0] = static_cast<std::uint8_t>(type);
    blob[1] = static_cast<std::uint8_t>(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
        blob[kFrameHeaderSize + i] = payload[i];
    }

    const std::size_t crc_offset = kFrameHeaderSize + payload_size;
    const std::uint16_t crc = crc16(blob, crc_offset);
    blob[crc_offset] = static_cast<std::uint8_t>(crc >> 8);          // CRC high byte first
    blob[crc_offset + 1] = static_cast<std::uint8_t>(crc & 0xFFu);   // then low byte
    const std::size_t blob_size = crc_offset + kFrameCrcSize;

    // Leave room for the trailing delimiter.
    const CobsResult cobs = cobs_encode(blob, blob_size, out, out_capacity - 1);
    if (cobs.status != CobsStatus::kOk) {
        return {EncodeStatus::kOutputOverflow, 0};
    }

    out[cobs.size] = 0x00;
    return {EncodeStatus::kOk, cobs.size + 1};
}

}  // namespace proto
