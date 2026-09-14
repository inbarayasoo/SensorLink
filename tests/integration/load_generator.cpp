// A synthetic device used only by tests/integration/run_integration_test.py
// -- not part of the shipped product, and not something a real cooling unit
// ever runs. Its only job is to make one subscriber's outgoing buffer fill
// up fast: it connects to the server's ingest port exactly like a real
// device (HELLO first), then blasts SAMPLE frames on its own metric for a
// fixed duration, as fast as the socket will take them.
//
// Why this needs to exist at all: server/connection.cpp's write-buffer
// high-water mark is 64 KiB, sized for a real deployment, not a fast test.
// A real device in this project samples at 2 Hz; reaching 64 KiB of
// buffered subscriber output at that rate would take on the order of half
// an hour. This tool reaches it in well under a second, by sending as many
// SAMPLE frames per second as loopback TCP allows -- something no real
// cold-chain sensor would ever do, and exactly why this lives under
// tests/integration/, not node/ (a real firmware task) or bench/ (a later
// stage, which will generalize this into a proper configurable load
// generator instead of this narrow, single-purpose one).

#include "proto/frame_encoder.hpp"
#include "proto/messages.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int connect_to_server(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

// --- for you to complete -------------------------------------------------
//
// Sends one HELLO (any device_id works -- pick something distinct from the
// real firmware's device_id of 1, e.g. 999, so the two are easy to tell
// apart if you ever add logging; offered_rate_hz can be anything too, since
// the server only caps what it *offers* in CONFIG -- it never polices how
// fast a device actually sends SAMPLE frames afterwards).
//
// Then loops until `duration` has elapsed, and on every iteration: builds a
// proto::SamplePayload for `metric_id`, encodes it with
// proto::encode_frame, and writes the result to fd with ::send. No delay
// between iterations -- send as fast as the loop runs. Bump timestamp_ms
// and value_milli by some fixed step each time so frames are not
// byte-for-byte identical (not required for this to work, just makes
// captured traffic easier to read if you ever look at it).
//
// This is the exact same shape as node/tasks/telemetry_task.cpp's own
// SAMPLE encoding -- proto::SamplePayload, proto::encode_frame, write the
// bytes out -- just a raw TCP socket standing in for the UART.
void run_load(int fd, std::uint8_t metric_id, std::chrono::seconds duration) {
    proto::HelloPayload hello{};
    hello.device_id = 999;
    hello.fw_version = proto::kProtocolVersion;
    hello.metric_count = 1;
    hello.offered_rate_hz = 100;

    std::uint8_t hello_frame[proto::max_encoded_frame_size(sizeof(hello))];
    const proto::EncodeResult hello_result = proto::encode_frame(
        proto::MessageType::kHello, reinterpret_cast<const std::uint8_t*>(&hello),
        sizeof(hello), hello_frame, sizeof(hello_frame));
    if (hello_result.status == proto::EncodeStatus::kOk) {
        ::send(fd, hello_frame, hello_result.size, MSG_NOSIGNAL);
    }

    const auto deadline = std::chrono::steady_clock::now() + duration;
    std::uint32_t timestamp_ms = 0;
    std::int32_t value_milli = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        proto::SamplePayload sample{};
        sample.session_id = 0;
        sample.timestamp_ms = timestamp_ms;
        sample.value_milli = value_milli;
        sample.metric_id = metric_id;

        std::uint8_t frame[proto::max_encoded_frame_size(sizeof(sample))];
        const proto::EncodeResult result = proto::encode_frame(
            proto::MessageType::kSample, reinterpret_cast<const std::uint8_t*>(&sample),
            sizeof(sample), frame, sizeof(frame));
        if (result.status == proto::EncodeStatus::kOk) {
            ::send(fd, frame, result.size, MSG_NOSIGNAL);
        }

        timestamp_ms += 10;
        value_milli += 1;
    }
}
// ---------------------------------------------------------------------------

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6000;
    std::uint8_t metric_id = 1;
    std::chrono::seconds duration{5};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) {
            const std::string endpoint = argv[++i];
            const std::size_t colon = endpoint.find(':');
            if (colon != std::string::npos) {
                host = endpoint.substr(0, colon);
                port = static_cast<std::uint16_t>(std::atoi(endpoint.substr(colon + 1).c_str()));
            }
        } else if (arg == "--metric" && i + 1 < argc) {
            metric_id = static_cast<std::uint8_t>(std::atoi(argv[++i]));
        } else if (arg == "--seconds" && i + 1 < argc) {
            duration = std::chrono::seconds(std::atoi(argv[++i]));
        }
    }

    const int fd = connect_to_server(host, port);
    if (fd < 0) {
        std::cerr << "sensorlink-load-generator: failed to connect to " << host << ":" << port << "\n";
        return 1;
    }

    run_load(fd, metric_id, duration);

    ::close(fd);
}
