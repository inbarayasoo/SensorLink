#include "server/connection.hpp"
#include "server/event_loop.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Mirrors event_loop_test.cpp's SocketPairTest: a connected pair of Unix
// sockets, closed automatically at teardown, standing in for a real TCP
// connection. fds_[0] is handed to the connection under test; fds_[1] plays
// the role of "the other side of the wire" that the test drives directly.
class ConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // SOCK_NONBLOCK: connection's contract requires the fd it is handed
        // to already be non-blocking (see connection.hpp) -- it never calls
        // fcntl() itself, that is the accepting code's job. socketpair()
        // applies the flag to both ends; that is harmless for fds_[1], which
        // the test only ever drives with a single small write()/read() that
        // never has to wait.
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds_), 0);
    }

    void TearDown() override {
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
        }
        // fds_[0] is owned by the connection under test once one is
        // constructed; connection's own destructor closes it.
    }

    int fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(ConnectionTest, TryReadDrainsEverythingAvailableInOneCall) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);

    const std::string payload = "HELLO device=cooler-7";
    ASSERT_EQ(::write(fds_[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));

    const bool still_open = conn.try_read();

    EXPECT_TRUE(still_open);
    ASSERT_EQ(conn.read_buffer().size(), payload.size());
    EXPECT_EQ(std::memcmp(conn.read_buffer().data(), payload.data(), payload.size()), 0);
}

TEST_F(ConnectionTest, ConsumeRemovesOnlyTheFrontBytes) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);

    const std::string payload = "ABCDEF";
    ASSERT_EQ(::write(fds_[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    ASSERT_TRUE(conn.try_read());

    conn.consume(3);

    ASSERT_EQ(conn.read_buffer().size(), 3u);
    EXPECT_EQ(std::memcmp(conn.read_buffer().data(), "DEF", 3), 0);
}

TEST_F(ConnectionTest, TryReadReturnsFalseWhenPeerCloses) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);

    ::close(fds_[1]);
    fds_[1] = -1;  // already closed; do not close again in TearDown

    const bool still_open = conn.try_read();

    EXPECT_FALSE(still_open);
    EXPECT_TRUE(conn.closed());
}

TEST_F(ConnectionTest, TryWriteDeliversQueuedBytesToThePeer) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);

    const std::string payload = "CONFIG rate=500ms";
    conn.enqueue_write(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());

    ASSERT_TRUE(conn.try_write());

    std::vector<char> received(payload.size());
    ASSERT_EQ(::read(fds_[1], received.data(), received.size()),
              static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(std::memcmp(received.data(), payload.data(), payload.size()), 0);
}

TEST_F(ConnectionTest, WriteCongestedBecomesTrueOnceHighWaterMarkIsCrossed) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);

    // Comfortably over any reasonable high-water mark (tens of KiB); queued
    // without a try_write() in between, so it sits entirely in the
    // connection's own buffer rather than draining to the kernel.
    const std::vector<std::uint8_t> chunk(128 * 1024, 0xAB);

    EXPECT_FALSE(conn.write_congested());
    conn.enqueue_write(chunk.data(), chunk.size());
    EXPECT_TRUE(conn.write_congested());
}
