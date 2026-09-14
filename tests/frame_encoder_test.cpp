#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "proto/cobs.hpp"
#include "proto/crc16.hpp"
#include "proto/frame_encoder.hpp"

using namespace proto;

namespace {

// Reverse the framing: check the delimiter, COBS-decode, return the blob
// (type | length | payload | crc16).
std::vector<std::uint8_t> unframe(const std::uint8_t* frame, std::size_t size) {
    EXPECT_GE(size, 1u);
    EXPECT_EQ(frame[size - 1], 0u);  // frame ends with the 0x00 delimiter
    for (std::size_t i = 0; i + 1 < size; ++i) {
        EXPECT_NE(frame[i], 0u);  // and contains no 0x00 anywhere else
    }
    std::vector<std::uint8_t> blob(size);
    const auto r = cobs_decode(frame, size - 1, blob.data(), blob.size());
    EXPECT_EQ(r.status, CobsStatus::kOk);
    blob.resize(r.status == CobsStatus::kOk ? r.size : 0);
    return blob;
}

}  // namespace

TEST(FrameEncoder, ProducesAWellFormedFrame) {
    const std::array<std::uint8_t, 6> payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    std::array<std::uint8_t, 64> out{};

    const auto r = encode_frame(MessageType::kSample, payload.data(), payload.size(),
                                out.data(), out.size());
    ASSERT_EQ(r.status, EncodeStatus::kOk);
    EXPECT_LE(r.size, max_encoded_frame_size(payload.size()));

    const std::vector<std::uint8_t> blob = unframe(out.data(), r.size);
    ASSERT_EQ(blob.size(), kFrameHeaderSize + payload.size() + kFrameCrcSize);
    EXPECT_EQ(blob[0], static_cast<std::uint8_t>(MessageType::kSample));
    EXPECT_EQ(blob[1], static_cast<std::uint8_t>(payload.size()));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(blob[kFrameHeaderSize + i], payload[i]);
    }
    // Residue over (blob without the CRC, then the CRC) is zero.
    EXPECT_EQ(crc16(blob.data(), blob.size()), 0u);
}

TEST(FrameEncoder, EmptyPayloadIsValid) {
    const std::array<std::uint8_t, 1> none{};
    std::array<std::uint8_t, 32> out{};

    const auto r = encode_frame(MessageType::kHeartbeat, none.data(), 0,
                                out.data(), out.size());
    ASSERT_EQ(r.status, EncodeStatus::kOk);

    const std::vector<std::uint8_t> blob = unframe(out.data(), r.size);
    ASSERT_EQ(blob.size(), kFrameHeaderSize + kFrameCrcSize);
    EXPECT_EQ(blob[0], static_cast<std::uint8_t>(MessageType::kHeartbeat));
    EXPECT_EQ(blob[1], 0u);
    EXPECT_EQ(crc16(blob.data(), blob.size()), 0u);
}

TEST(FrameEncoder, RejectsOversizedPayload) {
    const std::vector<std::uint8_t> big(kMaxPayloadSize + 1, 0xAB);
    std::array<std::uint8_t, 512> out{};
    const auto r = encode_frame(MessageType::kSample, big.data(), big.size(),
                                out.data(), out.size());
    EXPECT_EQ(r.status, EncodeStatus::kPayloadTooLarge);
}

TEST(FrameEncoder, ReportsOutputOverflow) {
    const std::array<std::uint8_t, 10> payload{};
    std::array<std::uint8_t, 4> tiny{};
    const auto r = encode_frame(MessageType::kSample, payload.data(), payload.size(),
                                tiny.data(), tiny.size());
    EXPECT_EQ(r.status, EncodeStatus::kOutputOverflow);
}

TEST(FrameEncoder, ReportsOutputOverflowOnZeroCapacity) {
    const std::array<std::uint8_t, 2> payload{0x01, 0x02};
    std::array<std::uint8_t, 1> out{};
    const auto r = encode_frame(MessageType::kSample, payload.data(), payload.size(),
                                out.data(), 0);
    EXPECT_EQ(r.status, EncodeStatus::kOutputOverflow);
}

TEST(FrameEncoder, MaxPayloadFitsInMaxEncodedFrameSize) {
    const std::vector<std::uint8_t> payload(kMaxPayloadSize, 0x00);  // worst case for COBS
    std::vector<std::uint8_t> out(max_encoded_frame_size(payload.size()));
    const auto r = encode_frame(MessageType::kSample, payload.data(), payload.size(),
                                out.data(), out.size());
    ASSERT_EQ(r.status, EncodeStatus::kOk);
    EXPECT_LE(r.size, out.size());
}

TEST(FrameEncoder, WorksInConstantExpression) {
    constexpr std::size_t size = [] {
        std::array<std::uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
        std::array<std::uint8_t, 32> out{};
        return encode_frame(MessageType::kConfig, payload.data(), payload.size(),
                            out.data(), out.size())
            .size;
    }();
    static_assert(size > 0, "frame encoding must work at compile time");
    SUCCEED();
}
