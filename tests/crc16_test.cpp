#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "proto/crc16.hpp"

namespace {

std::uint16_t crc_of(std::string_view s) {
    return proto::crc16(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace

TEST(Crc16, EmptyInputIsInitValue) {
    const std::array<std::uint8_t, 1> dummy{};
    EXPECT_EQ(proto::crc16(dummy.data(), 0), proto::kCrc16Init);
}

TEST(Crc16, CanonicalCheckVector) {
    // Documented check value for CRC-16/CCITT-FALSE over "123456789".
    EXPECT_EQ(crc_of("123456789"), 0x29B1u);
}

TEST(Crc16, IncrementalMatchesOneShot) {
    constexpr std::string_view msg = "SensorLink";
    std::uint16_t running = proto::kCrc16Init;
    for (char c : msg) {
        running = proto::crc16_update(running, static_cast<std::uint8_t>(c));
    }
    EXPECT_EQ(running, crc_of(msg));
}

TEST(Crc16, ByteOrderMatters) {
    EXPECT_NE(crc_of("AB"), crc_of("BA"));
}

TEST(Crc16, SingleBitFlipIsDetected) {
    std::array<std::uint8_t, 4> a{0x10, 0x20, 0x30, 0x40};
    std::array<std::uint8_t, 4> b = a;
    b[2] = static_cast<std::uint8_t>(b[2] ^ 0x01u);  // flip one bit
    EXPECT_NE(proto::crc16(a.data(), a.size()), proto::crc16(b.data(), b.size()));
}

TEST(Crc16, ResidueOfMessagePlusCrcIsZero) {
    const std::array<std::uint8_t, 5> msg{0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    const std::uint16_t c = proto::crc16(msg.data(), msg.size());

    std::array<std::uint8_t, 7> framed{};
    for (std::size_t i = 0; i < msg.size(); ++i) {
        framed[i] = msg[i];
    }
    framed[5] = static_cast<std::uint8_t>(c >> 8);      // CRC high byte first
    framed[6] = static_cast<std::uint8_t>(c & 0xFFu);

    EXPECT_EQ(proto::crc16(framed.data(), framed.size()), 0u);
}

TEST(Crc16, IsUsableInConstantExpressions) {
    constexpr std::array<std::uint8_t, 9> check{
        '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    static_assert(proto::crc16(check.data(), check.size()) == 0x29B1u,
                  "compile-time CRC must match the documented check value");
    SUCCEED();
}
