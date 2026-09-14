#include "server/event_loop.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>

namespace server {

namespace {
// epoll_wait fills at most this many events per call; a ready fd it could
// not fit this time is simply reported again on the next call, so this is a
// throughput knob, not a correctness one.
constexpr int kMaxEventsPerWait = 64;
}  // namespace

event_loop::event_loop() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (epoll_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

event_loop::~event_loop() {
    ::close(epoll_fd_);
}

void event_loop::add_timer(std::chrono::milliseconds delay, timer_callback cb) {
    timers_.push(timer{std::chrono::steady_clock::now() + delay, std::move(cb)});
}

void event_loop::fire_due_timers() {
    const auto now = std::chrono::steady_clock::now();
    while (!timers_.empty() && timers_.top().deadline <= now) {
        // Copy the due timer out and pop it before invoking the callback:
        // the callback may itself call add_timer() (a heartbeat rescheduling
        // itself is exactly this), which pushes onto the same priority_queue
        // we are mid-iteration over.
        timer due = timers_.top();
        timers_.pop();
        due.callback();
    }
}

int event_loop::poll() {
    int timeout_ms = -1;  // block indefinitely if no timer is pending
    if (!timers_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            timers_.top().deadline - now);
        timeout_ms = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, remaining.count()));
    }

    epoll_event ready[kMaxEventsPerWait];
    const int n = ::epoll_wait(epoll_fd_, ready, kMaxEventsPerWait, timeout_ms);
    if (n < 0) {
        // A signal delivered while blocked in epoll_wait is not a failure of
        // the loop -- there is simply nothing to dispatch this round.
        if (errno == EINTR) {
            fire_due_timers();
            return 0;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }

    for (int i = 0; i < n; ++i) {
        const auto it = callbacks_.find(ready[i].data.fd);
        if (it != callbacks_.end()) {
            it->second(ready[i].events);
        }
    }

    fire_due_timers();
    return n;
}

void event_loop::run() {
    running_ = true;
    while (running_) {
        poll();
    }
}

void event_loop::stop() {
    running_ = false;
}

void event_loop::add(int fd, std::uint32_t events, io_callback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
    }
    callbacks_[fd] = std::move(cb);
}

void event_loop::modify(int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl MOD");
    }
}

void event_loop::remove(int fd) {
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl DEL");
    }
    callbacks_.erase(fd);
}

}  // namespace server
