#include "server/event_loop.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

// A connected pair of Unix-domain sockets, closed automatically when the
// fixture goes out of scope. This stands in for a real TCP connection: it
// gives the event loop two real file descriptors it can epoll_wait on,
// without opening a network socket or depending on timing from an external
// process.
class SocketPairTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0);
    }

    void TearDown() override {
        ::close(fds_[0]);
        ::close(fds_[1]);
    }

    int fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(SocketPairTest, DispatchesCallbackWhenFdBecomesReadable) {
    server::event_loop loop;

    std::uint32_t seen_events = 0;
    int call_count = 0;
    loop.add(fds_[0], EPOLLIN, [&](std::uint32_t events) {
        seen_events = events;
        ++call_count;
    });

    // Nothing written yet: one iteration should find fds_[0] not ready, and
    // since there is no timer pending either, poll() would block forever --
    // so this assertion only checks state after data actually arrives.
    const char byte = 'x';
    ASSERT_EQ(::write(fds_[1], &byte, 1), 1);

    const int ready = loop.poll();

    EXPECT_EQ(ready, 1);
    EXPECT_EQ(call_count, 1);
    EXPECT_TRUE(seen_events & EPOLLIN);
}

TEST_F(SocketPairTest, RemoveStopsFurtherCallbacks) {
    server::event_loop loop;

    int call_count = 0;
    loop.add(fds_[0], EPOLLIN, [&](std::uint32_t) { ++call_count; });
    loop.remove(fds_[0]);

    const char byte = 'x';
    ASSERT_EQ(::write(fds_[1], &byte, 1), 1);

    // fds_[0] is unregistered, so epoll_wait has nothing to report; give it
    // a timer so poll() does not block the test indefinitely.
    loop.add_timer(std::chrono::milliseconds(10), [&loop] { loop.stop(); });
    const int ready = loop.poll();

    EXPECT_EQ(ready, 0);
    EXPECT_EQ(call_count, 0);
}

TEST(EventLoopTimers, FireInDeadlineOrderNotInsertionOrder) {
    server::event_loop loop;

    std::vector<int> fired_order;
    loop.add_timer(std::chrono::milliseconds(30), [&] { fired_order.push_back(2); });
    loop.add_timer(std::chrono::milliseconds(10), [&] { fired_order.push_back(1); });
    loop.add_timer(std::chrono::milliseconds(50), [&] {
        fired_order.push_back(3);
        loop.stop();
    });

    loop.run();

    EXPECT_EQ(fired_order, (std::vector<int>{1, 2, 3}));
}
