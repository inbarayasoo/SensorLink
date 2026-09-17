#include "server/store.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
constexpr auto kEpoch = std::chrono::steady_clock::time_point{};
}  // namespace

TEST(Store, HistoryIsEmptyForADeviceThatNeverRecordedAnything) {
    server::store data_store;

    EXPECT_TRUE(data_store.history(/*session_id=*/42, /*metric_id=*/0).empty());
}

TEST(Store, HistoryReturnsSamplesOldestFirstWhenBelowCapacity) {
    server::store data_store;

    data_store.record(/*session_id=*/1, /*metric_id=*/0, /*value_milli=*/1000, /*timestamp_ms=*/10, kEpoch);
    data_store.record(1, 0, 2000, 20, kEpoch);
    data_store.record(1, 0, 3000, 30, kEpoch);

    const std::vector<server::store::sample> history = data_store.history(1, 0);

    ASSERT_EQ(history.size(), 3u);
    EXPECT_EQ(history[0].timestamp_ms, 10u);
    EXPECT_EQ(history[1].timestamp_ms, 20u);
    EXPECT_EQ(history[2].timestamp_ms, 30u);
}

TEST(Store, HistoryDropsTheOldestSampleOnceCapacityIsExceeded) {
    server::store data_store;

    // One more than capacity: sample 0 (timestamp 0) must be the one that
    // gets overwritten, since it is the least recently written.
    for (std::size_t i = 0; i <= server::store::kHistoryPerDevice; ++i) {
        data_store.record(1, 0, static_cast<std::int32_t>(i), static_cast<std::uint32_t>(i), kEpoch);
    }

    const std::vector<server::store::sample> history = data_store.history(1, 0);

    ASSERT_EQ(history.size(), server::store::kHistoryPerDevice);
    EXPECT_EQ(history.front().timestamp_ms, 1u);  // 0 was dropped
    EXPECT_EQ(history.back().timestamp_ms, server::store::kHistoryPerDevice);
}

TEST(Store, DifferentMetricsOnTheSameDeviceHaveIndependentHistories) {
    server::store data_store;

    data_store.record(/*session_id=*/1, /*metric_id=*/0, 1111, 1, kEpoch);
    data_store.record(/*session_id=*/1, /*metric_id=*/1, 2222, 1, kEpoch);

    ASSERT_EQ(data_store.history(1, 0).size(), 1u);
    ASSERT_EQ(data_store.history(1, 1).size(), 1u);
    EXPECT_EQ(data_store.history(1, 0)[0].value_milli, 1111);
    EXPECT_EQ(data_store.history(1, 1)[0].value_milli, 2222);
}

TEST(Store, ThresholdCheckIsScopedToTheTempMetric) {
    server::store data_store;

    // metric_id 1 is pressure -- well above the temperature threshold, but
    // there is no threshold defined for pressure, so nothing should latch.
    for (int i = 0; i < 5; ++i) {
        data_store.record(1, /*metric_id=*/1, 9000, static_cast<std::uint32_t>(i), kEpoch);
    }

    EXPECT_FALSE(data_store.excursion_alert(1).active);
}

TEST(Store, ExcursionLatchesOnceTheSmoothedAverageCrossesTheThresholdNotTheRawValue) {
    server::store data_store;

    // Baseline average settles at 4000 (the first sample sets it directly).
    data_store.record(1, /*metric_id=*/0, 4000, 0, kEpoch);
    EXPECT_FALSE(data_store.excursion_alert(1).active);

    // Raw jumps to 9000, far above the 5500 threshold -- but the smoothed
    // average only moves a quarter of the way there: 4000 + (9000-4000)/4 =
    // 5250, still at or below the threshold. This is exactly the point of
    // moving the EMA server-side: a single noisy sample must not trip the
    // alert on its own.
    data_store.record(1, 0, 9000, 1, kEpoch);
    EXPECT_FALSE(data_store.excursion_alert(1).active);

    // A second sample at the same raw value keeps pulling the average up:
    // 5250 + (9000-5250)/4 = 6187, now past the threshold.
    data_store.record(1, 0, 9000, 2, kEpoch);
    EXPECT_TRUE(data_store.excursion_alert(1).active);
    EXPECT_EQ(data_store.excursion_alert(1).value_milli, 6187);
}

namespace {

// A minimal connected-socket fixture, matching connection_test.cpp, needed
// only because subscriber has no fake/mock form -- constructing one for
// real is the simplest way to observe record()'s fan-out.
class StoreFanOutTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, temp_fds_), 0);
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pressure_fds_), 0);
    }

    void TearDown() override {
        ::close(temp_fds_[1]);
        ::close(pressure_fds_[1]);
    }

    std::string ReadFrom(int fd) {
        char raw[256];
        const ssize_t n = ::read(fd, raw, sizeof(raw));
        if (n <= 0) {
            return {};
        }
        return std::string(raw, static_cast<std::size_t>(n));
    }

    int temp_fds_[2] = {-1, -1};
    int pressure_fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(StoreFanOutTest, RecordPushesToOnlyTheSubscriberWatchingThatMetric) {
    server::event_loop loop;
    server::store data_store;
    server::connection temp_conn(loop, temp_fds_[0]);
    server::connection pressure_conn(loop, pressure_fds_[0]);
    server::subscriber temp_sub(temp_conn, data_store);
    server::subscriber pressure_sub(pressure_conn, data_store);

    const std::string subscribe_temp = "SUBSCRIBE temp\n";
    ASSERT_EQ(::write(temp_fds_[1], subscribe_temp.data(), subscribe_temp.size()),
              static_cast<ssize_t>(subscribe_temp.size()));
    ASSERT_TRUE(temp_conn.try_read());
    temp_sub.on_readable();

    const std::string subscribe_pressure = "SUBSCRIBE pressure\n";
    ASSERT_EQ(::write(pressure_fds_[1], subscribe_pressure.data(), subscribe_pressure.size()),
              static_cast<ssize_t>(subscribe_pressure.size()));
    ASSERT_TRUE(pressure_conn.try_read());
    pressure_sub.on_readable();

    data_store.add_subscriber(&temp_sub);
    data_store.add_subscriber(&pressure_sub);

    data_store.record(/*session_id=*/1, /*metric_id=*/0 /* temp */, 5230, 1000, kEpoch);
    ASSERT_TRUE(temp_conn.try_write());
    ASSERT_TRUE(pressure_conn.try_write());

    EXPECT_EQ(ReadFrom(temp_fds_[1]), "temp 1 1000 5230\n");
    EXPECT_EQ(ReadFrom(pressure_fds_[1]), "");
}

TEST_F(StoreFanOutTest, RemoveSubscriberStopsFurtherUpdates) {
    server::event_loop loop;
    server::store data_store;
    server::connection temp_conn(loop, temp_fds_[0]);
    server::subscriber temp_sub(temp_conn, data_store);

    const std::string subscribe_temp = "SUBSCRIBE temp\n";
    ASSERT_EQ(::write(temp_fds_[1], subscribe_temp.data(), subscribe_temp.size()),
              static_cast<ssize_t>(subscribe_temp.size()));
    ASSERT_TRUE(temp_conn.try_read());
    temp_sub.on_readable();

    data_store.add_subscriber(&temp_sub);
    data_store.remove_subscriber(&temp_sub);

    data_store.record(1, 0, 5230, 1000, kEpoch);
    ASSERT_TRUE(temp_conn.try_write());

    EXPECT_EQ(ReadFrom(temp_fds_[1]), "");
    EXPECT_EQ(temp_sub.updates_sent(), 0u);
}

TEST_F(StoreFanOutTest, AddSubscriberReplaysAlreadyActiveAlertsToANewSubscriber) {
    server::event_loop loop;
    server::store data_store;

    // Latch an excursion before any subscriber exists at all.
    data_store.record(1, 0, 4000, 0, kEpoch);
    data_store.record(1, 0, 9000, 1, kEpoch);
    data_store.record(1, 0, 9000, 2, kEpoch);
    ASSERT_TRUE(data_store.excursion_alert(1).active);

    server::connection temp_conn(loop, temp_fds_[0]);
    server::subscriber temp_sub(temp_conn, data_store);

    // No record()/latch_offline() call happens here -- add_subscriber()
    // itself must be what delivers the already-open alert.
    data_store.add_subscriber(&temp_sub);
    ASSERT_TRUE(temp_conn.try_write());

    EXPECT_EQ(ReadFrom(temp_fds_[1]), "ALERT EXCURSION 1 2 6187\n");
}

namespace {

class StoreAckTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds_), 0);
    }

    void TearDown() override {
        ::close(fds_[1]);
    }

    std::string ReadFrom(int fd) {
        char raw[512];
        const ssize_t n = ::read(fd, raw, sizeof(raw));
        if (n <= 0) {
            return {};
        }
        return std::string(raw, static_cast<std::size_t>(n));
    }

    int fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(StoreAckTest, ExcursionDoesNotReLatchWhileAlreadyActive) {
    server::event_loop loop;
    server::store data_store;
    server::connection conn(loop, fds_[0]);
    server::subscriber sub(conn, data_store);
    data_store.add_subscriber(&sub);

    data_store.record(1, 0, 4000, 0, kEpoch);
    data_store.record(1, 0, 9000, 1, kEpoch);
    data_store.record(1, 0, 9000, 2, kEpoch);  // crosses -- one ALERT line
    data_store.record(1, 0, 9000, 3, kEpoch);  // still above -- no second line
    data_store.record(1, 0, 9000, 4, kEpoch);

    ASSERT_TRUE(conn.try_write());
    EXPECT_EQ(ReadFrom(fds_[1]), "ALERT EXCURSION 1 2 6187\n");
}

TEST_F(StoreAckTest, AckAfterGenuineRecoveryClosesTheExcursionAndAllowsARelatch) {
    server::store data_store;

    data_store.record(1, 0, 4000, 0, kEpoch);
    data_store.record(1, 0, 9000, 1, kEpoch);
    data_store.record(1, 0, 9000, 2, kEpoch);  // latches
    ASSERT_TRUE(data_store.excursion_alert(1).active);

    // Bring the smoothed average back down below the threshold before ack().
    // (excursion_alert().value_milli is frozen at the moment the alert
    // latched, not a live reading, so there is no public way to check the
    // current average directly -- the ack() result below is the real proof.)
    data_store.record(1, 0, 0, 3, kEpoch);
    data_store.record(1, 0, 0, 4, kEpoch);

    const auto result = data_store.ack(1, kEpoch + std::chrono::seconds(10));
    EXPECT_TRUE(result.had_excursion);
    EXPECT_FALSE(result.excursion_still_active);
    EXPECT_FALSE(data_store.excursion_alert(1).active);

    // A fresh crossing latches a brand new alert.
    data_store.record(1, 0, 9000, 5, kEpoch);
    data_store.record(1, 0, 9000, 6, kEpoch);
    EXPECT_TRUE(data_store.excursion_alert(1).active);
}

TEST_F(StoreAckTest, AckWhileStillExceedingTheThresholdReportsDurationWithoutResettingIt) {
    server::store data_store;

    data_store.record(1, 0, 4000, 0, kEpoch);
    data_store.record(1, 0, 9000, 1, kEpoch);
    data_store.record(1, 0, 9000, 2, kEpoch);  // latches, triggered_at = kEpoch
    ASSERT_TRUE(data_store.excursion_alert(1).active);

    // Still above threshold at ack time -- this is the regression case from
    // the design discussion: acknowledging an ongoing excursion must not
    // make it look like it just started.
    const auto first_ack = data_store.ack(1, kEpoch + std::chrono::seconds(5));
    EXPECT_TRUE(first_ack.had_excursion);
    EXPECT_TRUE(first_ack.excursion_still_active);
    EXPECT_EQ(first_ack.excursion_duration, std::chrono::seconds(5));
    EXPECT_TRUE(data_store.excursion_alert(1).active);  // NOT closed

    // Still above threshold, much later -- triggered_at must not have moved,
    // so this reports a much larger duration, not a small one.
    const auto second_ack = data_store.ack(1, kEpoch + std::chrono::hours(3));
    EXPECT_TRUE(second_ack.excursion_still_active);
    EXPECT_EQ(second_ack.excursion_duration, std::chrono::hours(3));
}

TEST_F(StoreAckTest, LatchOfflineNotifiesSubscribersAndIsIdempotent) {
    server::event_loop loop;
    server::store data_store;
    server::connection conn(loop, fds_[0]);
    server::subscriber sub(conn, data_store);
    data_store.add_subscriber(&sub);

    data_store.latch_offline(5, /*timestamp_ms=*/1000, kEpoch);
    data_store.latch_offline(5, 2000, kEpoch);  // already active -- no second line

    ASSERT_TRUE(conn.try_write());
    EXPECT_EQ(ReadFrom(fds_[1]), "ALERT OFFLINE 5 1000\n");
}

TEST_F(StoreAckTest, AckClearsBothExcursionAndOfflineTogetherForTheSameDevice) {
    server::store data_store;

    data_store.record(1, 0, 4000, 0, kEpoch);
    data_store.record(1, 0, 9000, 1, kEpoch);
    data_store.record(1, 0, 9000, 2, kEpoch);  // crosses -- excursion latches
    data_store.record(1, 0, 0, 3, kEpoch);     // recovers
    data_store.latch_offline(1, 500, kEpoch);

    ASSERT_TRUE(data_store.excursion_alert(1).active);
    ASSERT_TRUE(data_store.offline_alert(1).active);

    const auto result = data_store.ack(1, kEpoch + std::chrono::seconds(1));

    EXPECT_TRUE(result.had_excursion);
    EXPECT_TRUE(result.had_offline);
    EXPECT_FALSE(data_store.excursion_alert(1).active);
    EXPECT_FALSE(data_store.offline_alert(1).active);
}

TEST(Store, AckOnADeviceWithNoAlertsReturnsAllFalseWithoutCrashing) {
    server::store data_store;

    const auto result = data_store.ack(999, kEpoch);

    EXPECT_FALSE(result.had_excursion);
    EXPECT_FALSE(result.had_offline);
}
