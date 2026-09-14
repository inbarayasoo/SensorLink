#include "server/store.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

TEST(Store, HistoryIsEmptyForADeviceThatNeverRecordedAnything) {
    server::store data_store;

    EXPECT_TRUE(data_store.history(/*session_id=*/42, /*metric_id=*/0).empty());
}

TEST(Store, HistoryReturnsSamplesOldestFirstWhenBelowCapacity) {
    server::store data_store;

    data_store.record(/*session_id=*/1, /*metric_id=*/0, /*value_milli=*/1000, /*timestamp_ms=*/10);
    data_store.record(1, 0, 2000, 20);
    data_store.record(1, 0, 3000, 30);

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
        data_store.record(1, 0, static_cast<std::int32_t>(i),
                           static_cast<std::uint32_t>(i));
    }

    const std::vector<server::store::sample> history = data_store.history(1, 0);

    ASSERT_EQ(history.size(), server::store::kHistoryPerDevice);
    EXPECT_EQ(history.front().timestamp_ms, 1u);  // 0 was dropped
    EXPECT_EQ(history.back().timestamp_ms, server::store::kHistoryPerDevice);
}

TEST(Store, DifferentMetricsOnTheSameDeviceHaveIndependentHistories) {
    server::store data_store;

    data_store.record(/*session_id=*/1, /*metric_id=*/0, 1111, 1);
    data_store.record(/*session_id=*/1, /*metric_id=*/1, 2222, 1);

    ASSERT_EQ(data_store.history(1, 0).size(), 1u);
    ASSERT_EQ(data_store.history(1, 1).size(), 1u);
    EXPECT_EQ(data_store.history(1, 0)[0].value_milli, 1111);
    EXPECT_EQ(data_store.history(1, 1)[0].value_milli, 2222);
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
    server::connection temp_conn(loop, temp_fds_[0]);
    server::connection pressure_conn(loop, pressure_fds_[0]);
    server::subscriber temp_sub(temp_conn);
    server::subscriber pressure_sub(pressure_conn);

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

    server::store data_store;
    data_store.add_subscriber(&temp_sub);
    data_store.add_subscriber(&pressure_sub);

    data_store.record(/*session_id=*/1, /*metric_id=*/0 /* temp */, 5230, 1000);
    ASSERT_TRUE(temp_conn.try_write());
    ASSERT_TRUE(pressure_conn.try_write());

    EXPECT_EQ(ReadFrom(temp_fds_[1]), "temp 1000 5230\n");
    EXPECT_EQ(ReadFrom(pressure_fds_[1]), "");
}

TEST_F(StoreFanOutTest, RemoveSubscriberStopsFurtherUpdates) {
    server::event_loop loop;
    server::connection temp_conn(loop, temp_fds_[0]);
    server::subscriber temp_sub(temp_conn);

    const std::string subscribe_temp = "SUBSCRIBE temp\n";
    ASSERT_EQ(::write(temp_fds_[1], subscribe_temp.data(), subscribe_temp.size()),
              static_cast<ssize_t>(subscribe_temp.size()));
    ASSERT_TRUE(temp_conn.try_read());
    temp_sub.on_readable();

    server::store data_store;
    data_store.add_subscriber(&temp_sub);
    data_store.remove_subscriber(&temp_sub);

    data_store.record(1, 0, 5230, 1000);
    ASSERT_TRUE(temp_conn.try_write());

    EXPECT_EQ(ReadFrom(temp_fds_[1]), "");
    EXPECT_EQ(temp_sub.updates_sent(), 0u);
}
