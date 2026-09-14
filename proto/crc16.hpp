#pragma once

#include <cstddef>
#include <cstdint>

namespace proto {

// CRC-16/CCITT-FALSE parameters:
//   polynomial : 0x1021  (x^16 + x^12 + x^5 + 1)
//   init       : 0xFFFF
//   reflection : none (each byte is processed most-significant-bit first)
//   final xor  : 0x0000
// Residue is 0x0000: running crc16() over a message followed by its own CRC
// bytes in big-endian order yields 0. The frame parser relies on this.
inline constexpr std::uint16_t kCrc16Poly = 0x1021;
inline constexpr std::uint16_t kCrc16Init = 0xFFFF;

// Fold one byte into a running CRC. Seed the first call with kCrc16Init.
constexpr std::uint16_t crc16_update(std::uint16_t crc, std::uint8_t byte) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8));
    for (int i = 0; i < 8; ++i) {
        if ((crc & 0x8000u) != 0) {
            crc = static_cast<std::uint16_t>((crc << 1) ^ kCrc16Poly);
        } else {
            crc = static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

// CRC-16 over a contiguous buffer.
constexpr std::uint16_t crc16(const std::uint8_t* data, std::size_t len) {
    std::uint16_t crc = kCrc16Init;
    for (std::size_t i = 0; i < len; ++i) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

}  // namespace proto