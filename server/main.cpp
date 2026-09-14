// The real, running SensorLink server: opens the two TCP ports the rest of
// this project only ever simulates in tests (server_tests drives every
// class under server/ through a socketpair, never a real socket) and wires
// them into live connection/ingest/subscriber objects sharing one store.
//
// Every other file under server/ is policy or mechanism with no opinion on
// *when* it runs. This is the one place that decides that -- and the one
// piece of the whole cold-chain story that makes it real: without an accept
// loop actually listening on a port, no walk-in freezer, delivery truck, or
// pharmacy fridge could ever connect, and no QA dashboard could ever
// subscribe to their readings.
//
// Two listening sockets, one epoll loop, one shared store:
//   --ingest <port>     devices connect here and speak the binary,
//                       COBS-framed protocol (HELLO / SAMPLE / HEARTBEAT).
//   --subscribe <port>  dashboards and alerting clients connect here and
//                       speak the line-based text protocol (SUBSCRIBE /
//                       UNSUBSCRIBE / STATS).
//
// A single housekeeping timer, running once a second, does everything that
// does not need to happen the instant a byte arrives: drop connections the
// kernel already tore down, evict devices that stopped heartbeating, and --
// the piece that closes the backpressure loop end to end -- notice a
// congested subscriber and tell every connected device to slow down.

#include "server/connection.hpp"
#include "server/event_loop.hpp"
#include "server/ingest.hpp"
#include "server/store.hpp"
#include "server/subscriber.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {

constexpr std::chrono::milliseconds kHousekeepingInterval{1000};

// 3x the firmware's own heartbeat cadence (2s -- see
// node/tasks/command_task.cpp's kHeartbeatInterval): one delayed or dropped
// heartbeat must not evict a device that is still alive, but three missed
// in a row is a real disconnect, not a scheduling hiccup.
constexpr std::chrono::milliseconds kDeviceTimeout{6000};

// One accepted device connection, plus the protocol/session logic reading
// from it. Kept together so the pair is destroyed together the moment the
// device disconnects or times out.
struct DeviceSlot {
    std::unique_ptr<server::connection> conn;
    std::unique_ptr<server::ingest> ing;
};

// Same idea for one accepted dashboard/alerting client.
struct SubscriberSlot {
    std::unique_ptr<server::connection> conn;
    std::unique_ptr<server::subscriber> sub;
};

using DeviceMap = std::unordered_map<int, DeviceSlot>;
using SubscriberMap = std::unordered_map<int, SubscriberSlot>;

// --- for you to complete -----------------------------------------------
//
// Creates a non-blocking TCP socket, bound to `port` on every local
// interface, already listening. Returns the listening fd, or -1 if any
// step failed.
//
// This is the "raw sockets" half of the accept loop:
//   socket()      -- AF_INET, SOCK_STREAM, and SOCK_NONBLOCK or'd into the
//                     type (the same non-blocking contract connection.hpp's
//                     constructor already documents its fd must satisfy).
//   SO_REUSEADDR  -- via setsockopt(), so restarting the server right after
//                     a crash does not fail with "address already in use"
//                     while the old socket's TIME_WAIT lingers.
//   bind()        -- to INADDR_ANY:port.
//   listen()      -- with a reasonable backlog.
int make_listener(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }

    int reuse = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ::close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 16) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

// Drains every connection the kernel has queued on `listener_fd`: an
// accept4()-in-a-loop, the same shape as connection::try_read()'s recv()
// loop -- keep accepting until accept4() reports EAGAIN, since
// level-triggered epoll only promises "at least one is ready", never
// "exactly one".
//
// For every accepted socket: build a connection + ingest pair, wire the
// connection's on_readable hook to ingest.on_readable(), assign it
// next_session_id (then increment next_session_id), and store the pair in
// `devices` keyed by fd so housekeeping() can find it again.
void accept_devices(server::event_loop& loop, int listener_fd, server::store& data_store,
                     DeviceMap& devices, std::uint16_t& next_session_id) {
    while (true) {
        const int fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0) {
            break;
        }

        DeviceSlot slot;
        slot.conn = std::make_unique<server::connection>(loop, fd);
        slot.ing = std::make_unique<server::ingest>(*slot.conn, next_session_id, data_store);
        ++next_session_id;
        slot.conn->on_readable([ing = slot.ing.get()] { ing->on_readable(); });

        devices.emplace(fd, std::move(slot));
    }
}
// -------------------------------------------------------------------------

// Same shape as accept_devices, for the subscriber listener: no session id
// to hand out, and the subscriber has to register itself with data_store so
// store::record() actually fans reads out to it.
void accept_subscribers(server::event_loop& loop, int listener_fd, server::store& data_store,
                         SubscriberMap& subscribers) {
    while (true) {
        const int fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0) {
            break;
        }

        SubscriberSlot slot;
        slot.conn = std::make_unique<server::connection>(loop, fd);
        slot.sub = std::make_unique<server::subscriber>(*slot.conn);
        slot.conn->on_readable([sub = slot.sub.get()] { sub->on_readable(); });
        data_store.add_subscriber(slot.sub.get());

        subscribers.emplace(fd, std::move(slot));
    }
}

void housekeeping(server::event_loop& loop, server::store& data_store,
                   DeviceMap& devices, SubscriberMap& subscribers) {
    const auto now = std::chrono::steady_clock::now();

    bool any_subscriber_congested = false;
    for (auto it = subscribers.begin(); it != subscribers.end();) {
        if (it->second.conn->closed()) {
            data_store.remove_subscriber(it->second.sub.get());
            it = subscribers.erase(it);
            continue;
        }
        if (it->second.conn->write_congested()) {
            any_subscriber_congested = true;
        }
        ++it;
    }

    for (auto it = devices.begin(); it != devices.end();) {
        const bool disconnected = it->second.conn->closed();
        const bool timed_out = it->second.ing->has_session() &&
                                it->second.ing->session()->timed_out(now, kDeviceTimeout);
        if (disconnected || timed_out) {
            it = devices.erase(it);
            continue;
        }

        if (any_subscriber_congested && it->second.ing->has_session()) {
            // Halve the rate, never all the way to 0 -- a silenced device
            // has no way to be told to speed back up, and the heartbeat it
            // keeps sending at any nonzero rate is how the server tells a
            // slow device apart from a dead one.
            const std::uint8_t current = it->second.ing->session()->current_rate_hz();
            const std::uint8_t slower = std::max<std::uint8_t>(1, static_cast<std::uint8_t>(current / 2));
            it->second.ing->send_slow_down(slower);
        }
        ++it;
    }

    loop.add_timer(kHousekeepingInterval, [&loop, &data_store, &devices, &subscribers] {
        housekeeping(loop, data_store, devices, subscribers);
    });
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t ingest_port = 6000;
    std::uint16_t subscribe_port = 6001;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ingest" && i + 1 < argc) {
            ingest_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--subscribe" && i + 1 < argc) {
            subscribe_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        }
    }

    const int ingest_fd = make_listener(ingest_port);
    const int subscribe_fd = make_listener(subscribe_port);
    if (ingest_fd < 0 || subscribe_fd < 0) {
        std::cerr << "sensorlink-server: failed to open listening sockets\n";
        return 1;
    }

    server::event_loop loop;
    server::store data_store;
    DeviceMap devices;
    SubscriberMap subscribers;
    std::uint16_t next_session_id = 1;

    loop.add(ingest_fd, EPOLLIN, [&](std::uint32_t) {
        accept_devices(loop, ingest_fd, data_store, devices, next_session_id);
    });
    loop.add(subscribe_fd, EPOLLIN, [&](std::uint32_t) {
        accept_subscribers(loop, subscribe_fd, data_store, subscribers);
    });
    loop.add_timer(kHousekeepingInterval, [&loop, &data_store, &devices, &subscribers] {
        housekeeping(loop, data_store, devices, subscribers);
    });

    std::cout << "sensorlink-server listening: ingest=" << ingest_port
              << " subscribe=" << subscribe_port << std::endl;

    loop.run();
}
