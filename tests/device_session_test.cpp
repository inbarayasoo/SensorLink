#include "server/device_session.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace {

// device_session never calls steady_clock::now() itself -- every method
// that cares about time takes it as a parameter. That is what lets these
// tests move time forward instantly instead of actually sleeping.
constexpr auto kEpoch = std::chrono::steady_clock::time_point{};

proto::HelloPayload MakeHello(std::uint8_t offered_rate_hz, std::uint8_t metric_count) {
    return proto::HelloPayload{/*device_id=*/0x1234, /*fw_version=*/1,
                                metric_count, offered_rate_hz};
}

}  // namespace

TEST(DeviceSession, HandleHelloPassesThroughARateBelowTheServerCap) {
    server::device_session session(/*session_id=*/7, kEpoch);

    const proto::ConfigPayload config = session.handle_hello(MakeHello(2, 1));

    EXPECT_EQ(config.session_id, 7);
    EXPECT_EQ(config.sample_rate_hz, 2);
    EXPECT_EQ(session.current_rate_hz(), 2);
}

TEST(DeviceSession, HandleHelloCapsARateAboveTheServerMaximum) {
    server::device_session session(/*session_id=*/1, kEpoch);

    // 200 Hz is not a realistic offer for a temperature probe -- this is
    // exactly the "device lies about its capabilities" case the cap exists
    // to contain.
    const proto::ConfigPayload config = session.handle_hello(MakeHello(200, 1));

    EXPECT_LT(config.sample_rate_hz, 200);
    EXPECT_EQ(session.current_rate_hz(), config.sample_rate_hz);
}

TEST(DeviceSession, HandleHelloSetsOneMaskBitPerOfferedMetric) {
    server::device_session session(/*session_id=*/1, kEpoch);

    const proto::ConfigPayload config = session.handle_hello(MakeHello(1, 3));

    EXPECT_EQ(config.metric_mask, 0b0000'0111);
}

TEST(DeviceSession, HandleHelloClampsMetricMaskToEightBits) {
    server::device_session session(/*session_id=*/1, kEpoch);

    // metric_mask is a single byte -- offering more than 8 metrics cannot be
    // represented, so everything beyond bit 7 is simply not set rather than
    // overflowing or being undefined behaviour.
    const proto::ConfigPayload config = session.handle_hello(MakeHello(1, 20));

    EXPECT_EQ(config.metric_mask, 0xFF);
}

TEST(DeviceSession, NotTimedOutBeforeTheTimeoutElapses) {
    server::device_session session(/*session_id=*/1, kEpoch);

    EXPECT_FALSE(session.timed_out(kEpoch + std::chrono::milliseconds(400),
                                    std::chrono::milliseconds(500)));
}

TEST(DeviceSession, TimedOutAfterTheTimeoutElapsesWithNoActivity) {
    server::device_session session(/*session_id=*/1, kEpoch);

    EXPECT_TRUE(session.timed_out(kEpoch + std::chrono::milliseconds(600),
                                   std::chrono::milliseconds(500)));
}

TEST(DeviceSession, NoteActivityPostponesTheTimeout) {
    server::device_session session(/*session_id=*/1, kEpoch);

    session.note_activity(kEpoch + std::chrono::milliseconds(400));

    EXPECT_FALSE(session.timed_out(kEpoch + std::chrono::milliseconds(700),
                                    std::chrono::milliseconds(500)));
}

TEST(DeviceSession, SlowDownUpdatesTheCurrentRateAndReturnsAMatchingPayload) {
    server::device_session session(/*session_id=*/42, kEpoch);
    session.handle_hello(MakeHello(5, 1));

    const proto::SlowDownPayload payload = session.slow_down(1);

    EXPECT_EQ(payload.session_id, 42);
    EXPECT_EQ(payload.new_rate_hz, 1);
    EXPECT_EQ(session.current_rate_hz(), 1);
}

TEST(DeviceSession, NoteUptimeIsReadableViaLastUptimeMs) {
    server::device_session session(/*session_id=*/1, kEpoch);

    session.note_uptime(1234);

    EXPECT_EQ(session.last_uptime_ms(), 1234u);
}
