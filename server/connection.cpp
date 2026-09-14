#include "server/connection.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace server {

namespace {
// Above this many buffered-but-unsent bytes, a connection is "congested"
// and stops accepting more work on its own socket until it drains. 64 KiB
// tracks the kernel's own default TCP send-buffer size on Linux
// (net.core.wmem_default; auto-tuned but typically tens of KiB) -- picking
// roughly that order of magnitude means this cap starts biting at about the
// same point send() itself would start returning EAGAIN anyway, instead of
// throttling earlier than the kernel already would. A per-connection cap
// also bounds worst-case memory linearly in the number of connections: 300
// congested connections at 64 KiB each is about 19 MiB, not unbounded.
constexpr std::size_t kWriteHighWaterMark = 64 * 1024;
}  // namespace

connection::connection(event_loop& loop, int fd) : loop_(loop), fd_(fd) {
    loop_.add(fd_, EPOLLIN, [this](std::uint32_t events) {
        if (events & EPOLLIN) {
            try_read();
            if (on_readable_) {
                on_readable_();
            }
        }
        if (events & EPOLLOUT) {
            try_write();
        }
    });
}

connection::~connection() {
    loop_.remove(fd_);
    ::close(fd_);
}

void connection::consume(std::size_t n) {
    read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<long>(n));
}

void connection::enqueue_write(const std::uint8_t* data, std::size_t len) {
    write_buffer_.insert(write_buffer_.end(), data, data + len);
    write_interest_ = true;
    update_epoll_interest();
}

bool connection::write_congested() const {
    return write_buffer_.size() >= kWriteHighWaterMark;
}

void connection::update_epoll_interest() {
    std::uint32_t events = 0;
    if (!write_congested()) {
        events |= EPOLLIN;
    }
    if (write_interest_) {
        events |= EPOLLOUT;
    }
    loop_.modify(fd_, events);
}

bool connection::try_read() {
    while (true) {
        std::uint8_t chunk[4096];
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);

        if (n > 0) {
            read_buffer_.insert(read_buffer_.end(), chunk, chunk + n);
            continue;
        }
        if (n == 0) {
            closed_ = true;
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        closed_ = true;
        return false;
    }
}

bool connection::try_write() {
    while (!write_buffer_.empty()) {
        const ssize_t n = ::send(fd_, write_buffer_.data(), write_buffer_.size(), MSG_NOSIGNAL);

        if (n > 0) {
            write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        closed_ = true;
        return false;
    }

    write_interest_ = !write_buffer_.empty();
    update_epoll_interest();
    return true;
}

}  // namespace server
