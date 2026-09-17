#include "server/subscriber.hpp"

#include "server/store.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>

#include <gtest/gtest.h>

namespace {

// Same socketpair fixture as connection_test.cpp and ingest_test.cpp:
// fds_[0] is owned by the connection under test, fds_[1] plays the wire's
// other end -- a dashboard, in this file's tests.
class SubscriberTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds_), 0);
    }

    void TearDown() override {
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
        }
    }

    void SendLine(const std::string& line) {
        ASSERT_EQ(::write(fds_[1], line.data(), line.size()), static_cast<ssize_t>(line.size()));
    }

    // Reads whatever the server has written back so far, as plain text.
    std::string ReadFromServer() {
        char raw[256];
        const ssize_t n = ::read(fds_[1], raw, sizeof(raw));
        if (n <= 0) {
            return {};
        }
        return std::string(raw, static_cast<std::size_t>(n));
    }

    int fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(SubscriberTest, SubscribeSetsTheSubscribedMetric) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("SUBSCRIBE temp\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_TRUE(sub.subscribed());
    EXPECT_EQ(sub.subscribed_metric(), "temp");
}

TEST_F(SubscriberTest, UnsubscribeClearsTheSubscribedMetric) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("SUBSCRIBE temp\nUNSUBSCRIBE\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_FALSE(sub.subscribed());
}

TEST_F(SubscriberTest, CrlfLineEndingIsTolerated) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("SUBSCRIBE temp\r\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_EQ(sub.subscribed_metric(), "temp");
}

TEST_F(SubscriberTest, ALineArrivingSplitAcrossTwoReadsIsStillParsedCorrectly) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("SUB");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();
    EXPECT_FALSE(sub.subscribed());  // no newline yet -- nothing to act on

    SendLine("SCRIBE pressure\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_EQ(sub.subscribed_metric(), "pressure");
}

TEST_F(SubscriberTest, PublishSendsAnUpdateOnlyForTheSubscribedMetric) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("SUBSCRIBE temp\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    sub.publish("pressure", /*session_id=*/1, /*value_milli=*/1000, /*timestamp_ms=*/1);
    sub.publish("temp", /*session_id=*/3, /*value_milli=*/5230, /*timestamp_ms=*/2000);
    ASSERT_TRUE(conn.try_write());

    EXPECT_EQ(ReadFromServer(), "temp 3 2000 5230\n");
    EXPECT_EQ(sub.updates_sent(), 1u);
}

TEST_F(SubscriberTest, StatsReportsTheSubscribedMetricAndUpdateCount) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("SUBSCRIBE temp\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();
    sub.publish("temp", /*session_id=*/1, 1000, 1);

    SendLine("STATS\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();
    ASSERT_TRUE(conn.try_write());

    EXPECT_EQ(ReadFromServer(), "temp 1 1 1000\nSTATS metric=temp updates_sent=1\n");
}

TEST_F(SubscriberTest, PublishExcursionAlertIgnoresSubscribedMetric) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    // Subscribed to pressure, not temp -- an alert is not a per-metric feed
    // update, so it must still arrive regardless.
    SendLine("SUBSCRIBE pressure\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    sub.publish_excursion_alert(/*session_id=*/3, /*value_milli=*/5600, /*timestamp_ms=*/1000);
    ASSERT_TRUE(conn.try_write());

    EXPECT_EQ(ReadFromServer(), "ALERT EXCURSION 3 1000 5600\n");
}

TEST_F(SubscriberTest, AckWithAValidSessionIdCallsStoreAckAndRepliesAckOk) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    // Drive an excursion into store directly, the same way ingest would,
    // then bring the smoothed reading back down below the threshold --
    // ACK only ever fully closes an excursion that has genuinely recovered
    // by the time it is called (see store.hpp's ack() contract).
    data_store.record(/*session_id=*/3, /*metric_id=*/0, /*value_milli=*/9000,
                       /*timestamp_ms=*/1000, std::chrono::steady_clock::now());
    ASSERT_TRUE(data_store.excursion_alert(3).active);
    data_store.record(3, 0, 0, 1001, std::chrono::steady_clock::now());
    data_store.record(3, 0, 0, 1002, std::chrono::steady_clock::now());

    SendLine("ACK 3\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();
    ASSERT_TRUE(conn.try_write());

    EXPECT_EQ(ReadFromServer(), "ACK_OK 3 excursion_ms=0 excursion_active=0 offline_ms=0\n");
    EXPECT_FALSE(data_store.excursion_alert(3).active);
}

TEST_F(SubscriberTest, MalformedAckIsIgnoredWithoutCrashing) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::subscriber sub(conn, data_store);

    SendLine("ACK\nACK notanumber\nACK 3 4\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    // try_write() on an empty buffer trivially succeeds -- what actually
    // proves nothing was enqueued is that the peer receives zero bytes.
    ASSERT_TRUE(conn.try_write());
    EXPECT_EQ(ReadFromServer(), "");
}
