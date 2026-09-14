#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace server {

// A single-threaded epoll event loop: the one place in the server that ever
// calls epoll_wait. Every other component (connection, device_session,
// ingest, subscriber) registers a file descriptor here and gets a callback
// when the kernel says it is ready, instead of blocking a thread on it.
//
// This is what lets one process hold hundreds of device and subscriber
// connections at once. A thread-per-connection server would need one OS
// thread per cooling unit reporting in -- fine for a handful of fridges, but
// it does not scale to a real cold-chain deployment with hundreds of trucks
// and warehouses, and a slow thread stuck on one socket cannot easily be
// told "temporarily stop paying attention to that other fd" the way this
// loop can with modify().
class event_loop {
public:
    // Invoked with the ready event mask (EPOLLIN, EPOLLOUT, ... -- may be
    // several bits at once) whenever epoll reports fd as ready.
    using io_callback = std::function<void(std::uint32_t events)>;

    // Invoked once, when the timer's deadline is reached.
    using timer_callback = std::function<void()>;

    event_loop();
    ~event_loop();

    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    // Starts watching fd for `events` (an EPOLLIN / EPOLLOUT / ... bitmask).
    // cb is stored and called from poll() every time fd becomes ready.
    void add(int fd, std::uint32_t events, io_callback cb);

    // Changes which events fd is watched for. This is the mechanism
    // backpressure is built on one layer up: once a connection's outgoing
    // buffer is full, it starts watching EPOLLOUT on its own socket (so it
    // learns the moment the kernel can accept more) and stops watching
    // EPOLLIN on whichever fd is feeding it data it cannot yet send.
    void modify(int fd, std::uint32_t events);

    // Stops watching fd. Does not close it -- whoever owns the socket
    // (connection) is responsible for that, in whichever order its own
    // teardown needs.
    void remove(int fd);

    // Schedules cb to run once, no earlier than `delay` from now. Used for
    // heartbeats (send one every 2s) and idle timeouts (close a connection
    // that has gone quiet).
    void add_timer(std::chrono::milliseconds delay, timer_callback cb);

    // Runs one iteration: blocks in epoll_wait until either an fd is ready
    // or the earliest pending timer is due, dispatches every ready fd's
    // callback, then fires every timer whose deadline has passed. Returns
    // the number of fds epoll_wait reported ready (0 on a pure timer wakeup
    // or a signal interruption).
    int poll();

    // Calls poll() in a loop until stop() is called from within a callback.
    void run();

    void stop();

private:
    struct timer {
        std::chrono::steady_clock::time_point deadline;
        timer_callback callback;
    };

    // std::priority_queue is a max-heap by default; this comparator flips
    // the order so the *earliest* deadline sits at top() -- that is the
    // next timer poll() needs to wait for or fire.
    struct timer_later {
        bool operator()(const timer& a, const timer& b) const {
            return a.deadline > b.deadline;
        }
    };

    void fire_due_timers();

    int epoll_fd_;
    bool running_ = false;
    std::unordered_map<int, io_callback> callbacks_;
    std::priority_queue<timer, std::vector<timer>, timer_later> timers_;
};

}  // namespace server
