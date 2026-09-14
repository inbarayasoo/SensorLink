#pragma once

#include <cstddef>
#include <cstdint>

// Consistent Overhead Byte Stuffing.
//
// COBS rewrites a payload so that the byte value 0x00 never appears in the
// result. The framing layer can then use a single 0x00 as an unambiguous frame
// delimiter. Overhead is bounded: one byte, plus one more for every 254
// consecutive non-zero bytes.
//
// The functions here operate on caller-provided buffers and never allocate, so
// the same code runs unchanged on the microcontroller. cobs_encode() does NOT
// append the 0x00 delimiter; that is the frame encoder's job.
namespace proto {

enum class CobsStatus : std::uint8_t {
    kOk,
    kOutputOverflow,  // destination buffer is too small
    kInvalidInput,    // decode only: a 0x00 byte, or a block that runs past the end
};

struct CobsResult {
    CobsStatus status;
    std::size_t size;  // bytes written to dst; meaningful only when status == kOk
};

// Upper bound on the encoded size of a raw payload of the given length.
constexpr std::size_t cobs_encoded_max_size(std::size_t raw_size) noexcept {
    return raw_size + raw_size / 254 + 1;
}

// Encode [src, src + src_size) into dst. On success returns {kOk, encoded_size}.
constexpr CobsResult cobs_encode(const std::uint8_t* src, std::size_t src_size,
                                 std::uint8_t* dst, std::size_t dst_capacity) noexcept {
    if (dst_capacity == 0) {
        return {CobsStatus::kOutputOverflow, 0};
    }

    std::size_t write = 1;        // dst[0] is reserved for the first code byte
    std::size_t code_index = 0;   // where the current code byte will be written
    std::uint8_t code = 1;        // 1 + number of non-zero bytes in the block

    for (std::size_t read = 0; read < src_size; ++read) {
        const std::uint8_t byte = src[read];
        if (byte != 0) {
            if (write >= dst_capacity) {
                return {CobsStatus::kOutputOverflow, 0};
            }
            dst[write++] = byte;
            ++code;
            if (code == 0xFF) {  // block is full: close it and start another
                dst[code_index] = code;
                if (write >= dst_capacity) {
                    return {CobsStatus::kOutputOverflow, 0};
                }
                code_index = write++;
                code = 1;
            }
        } else {  // a zero ends the current block
            dst[code_index] = code;
            if (write >= dst_capacity) {
                return {CobsStatus::kOutputOverflow, 0};
            }
            code_index = write++;
            code = 1;
        }
    }

    dst[code_index] = code;  // close the final block
    return {CobsStatus::kOk, write};
}

// Decode [src, src + src_size) into dst. src must be the COBS data only, with no
// trailing 0x00 delimiter. On success returns {kOk, decoded_size}.
constexpr CobsResult cobs_decode(const std::uint8_t* src, std::size_t src_size,
                                 std::uint8_t* dst, std::size_t dst_capacity) noexcept {
    std::size_t write = 0;
    std::size_t read = 0;

    while (read < src_size) {
        const std::uint8_t code = src[read++];
        if (code == 0) {
            return {CobsStatus::kInvalidInput, 0};
        }

        for (std::uint8_t i = 1; i < code; ++i) {
            if (read >= src_size) {
                return {CobsStatus::kInvalidInput, 0};  // block runs past the end
            }
            const std::uint8_t byte = src[read++];
            if (byte == 0) {
                return {CobsStatus::kInvalidInput, 0};
            }
            if (write >= dst_capacity) {
                return {CobsStatus::kOutputOverflow, 0};
            }
            dst[write++] = byte;
        }

        // Every block except a full (0xFF) one implies a trailing zero, but the
        // final block does not: its zero is the frame delimiter, not data.
        if (code != 0xFF && read < src_size) {
            if (write >= dst_capacity) {
                return {CobsStatus::kOutputOverflow, 0};
            }
            dst[write++] = 0;
        }
    }

    return {CobsStatus::kOk, write};
}

}  // namespace proto