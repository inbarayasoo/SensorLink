#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "proto/messages.hpp"

using namespace proto;

TEST(Messages, TypeValuesAreStable) {
    EXPECT_EQ(static_cast<std::uint8_t>(MessageType::kHello), 1u);
    EXPECT_EQ(static_cast<std::uint8_t>(MessageType::kConfig), 2u);
    EXPECT_EQ(static_cast<std::uint8_t>(MessageType::kSample), 3u);
    EXPECT_EQ(static_cast<std::uint8_t>(MessageType::kSlowDown), 4u);
    EXPECT_EQ(static_cast<std::uint8_t>(MessageType::kHeartbeat), 5u);
}

TEST(Messages, StructsArePackedToTheExpectedSize) {
    EXPECT_EQ(sizeof(FrameHeader), 2u);
    EXPECT_EQ(sizeof(HelloPayload), 8u);
    EXPECT_EQ(sizeof(ConfigPayload), 4u);
    EXPECT_EQ(sizeof(SamplePayload), 11u);
    EXPECT_EQ(sizeof(SlowDownPayload), 3u);
    EXPECT_EQ(sizeof(HeartbeatPayload), 6u);
}

TEST(Messages, PackedLayoutHasNoPadding) {
    EXPECT_EQ(sizeof(SamplePayload),
              sizeof(std::uint16_t) + sizeof(std::uint32_t) +
                  sizeof(std::int32_t) + sizeof(std::uint8_t));
}

TEST(Messages, SampleFieldOffsetsMatchTheWireLayout) {
    EXPECT_EQ(offsetof(SamplePayload, session_id), 0u);
    EXPECT_EQ(offsetof(SamplePayload, timestamp_ms), 2u);
    EXPECT_EQ(offsetof(SamplePayload, value_milli), 6u);
    EXPECT_EQ(offsetof(SamplePayload, metric_id), 10u);
}
