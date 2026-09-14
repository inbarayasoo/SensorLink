#include "server/subscriber.hpp"

#include <sys/socket.h>
#include <unistd.h>

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
    server::subscriber sub(conn);

    SendLine("SUBSCRIBE temp\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_TRUE(sub.subscribed());
    EXPECT_EQ(sub.subscribed_metric(), "temp");
}

TEST_F(SubscriberTest, UnsubscribeClearsTheSubscribedMetric) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::subscriber sub(conn);

    SendLine("SUBSCRIBE temp\nUNSUBSCRIBE\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_FALSE(sub.subscribed());
}

TEST_F(SubscriberTest, CrlfLineEndingIsTolerated) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::subscriber sub(conn);

    SendLine("SUBSCRIBE temp\r\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    EXPECT_EQ(sub.subscribed_metric(), "temp");
}

TEST_F(SubscriberTest, ALineArrivingSplitAcrossTwoReadsIsStillParsedCorrectly) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::subscriber sub(conn);

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
    server::subscriber sub(conn);

    SendLine("SUBSCRIBE temp\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();

    sub.publish("pressure", /*value_milli=*/1000, /*timestamp_ms=*/1);
    sub.publish("temp", /*value_milli=*/5230, /*timestamp_ms=*/2000);
    ASSERT_TRUE(conn.try_write());

    EXPECT_EQ(ReadFromServer(), "temp 2000 5230\n");
    EXPECT_EQ(sub.updates_sent(), 1u);
}

TEST_F(SubscriberTest, StatsReportsTheSubscribedMetricAndUpdateCount) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::subscriber sub(conn);

    SendLine("SUBSCRIBE temp\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();
    sub.publish("temp", 1000, 1);

    SendLine("STATS\n");
    ASSERT_TRUE(conn.try_read());
    sub.on_readable();
    ASSERT_TRUE(conn.try_write());

    EXPECT_EQ(ReadFromServer(), "temp 1 1000\nSTATS metric=temp updates_sent=1\n");
}
