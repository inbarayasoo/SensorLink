#pragma once

#include "server/event_loop.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace server {

// A single non-blocking TCP socket plus the buffering a non-blocking socket
// always needs: recv() and send() only ever move "as much as fits right
// now" -- never a guaranteed all-or-nothing amount. connection is the layer
// between a raw fd and everything above it: device_session and ingest read
// framed device bytes out of read_buffer(); device_session's CONFIG /
// SLOW_DOWN replies and the subscriber's live feed go out through
// enqueue_write().
//
// One connection exists per accepted socket -- one per cooling unit
// reporting in, one per dashboard watching a feed.
class connection {
public:
    // fd must already be a connected, non-blocking socket (accept4() with
    // SOCK_NONBLOCK). connection takes ownership: it closes fd in its
    // destructor and deregisters it from loop before doing so.
    connection(event_loop& loop, int fd);
    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    // Drains everything the kernel currently has buffered for this socket
    // into the internal read buffer, looping until EAGAIN or the peer
    // closing the connection. Returns false once the peer has closed or a
    // fatal error occurred -- read_buffer() still holds whatever arrived
    // before that.
    bool try_read();

    // Bytes accumulated by try_read() and not yet consumed.
    const std::vector<std::uint8_t>& read_buffer() const { return read_buffer_; }

    // Removes the first n bytes from the front of the read buffer, once a
    // higher layer (ingest's frame_parser) has consumed them.
    void consume(std::size_t n);

    // Appends data to the outgoing buffer and starts watching EPOLLOUT if it
    // was not already being watched.
    void enqueue_write(const std::uint8_t* data, std::size_t len);

    // Flushes as much of the outgoing buffer as the kernel will currently
    // accept, looping until EAGAIN or the buffer empties. Stops watching
    // EPOLLOUT again once there is nothing left to flush.
    bool try_write();

    // True once the outgoing buffer has grown past a high-water mark: the
    // peer is not draining it as fast as data is being queued. While this is
    // true, connection stops watching EPOLLIN on itself, so it does not keep
    // accepting more work it cannot yet turn into outgoing bytes.
    bool write_congested() const;

    // True once try_read()/try_write() have observed the peer close the
    // connection or a fatal socket error. The owner (built in a later
    // sub-section) is responsible for destroying the connection once this
    // is true.
    bool closed() const { return closed_; }

    int fd() const { return fd_; }

    // Called after every try_read() the internal EPOLLIN callback triggers
    // -- lets an owner (ingest) react to newly-arrived bytes instead of
    // polling read_buffer() itself. Unset by default, so code that only
    // cares about the buffers directly is unaffected.
    void on_readable(std::function<void()> cb) { on_readable_ = std::move(cb); }

private:
    void update_epoll_interest();

    event_loop& loop_;
    int fd_;
    bool write_interest_ = false;
    bool closed_ = false;

    std::vector<std::uint8_t> read_buffer_;
    std::vector<std::uint8_t> write_buffer_;
    std::function<void()> on_readable_;
};

}  // namespace server
