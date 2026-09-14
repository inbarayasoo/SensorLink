// Load-testing tool for bench/run_benchmark.py: simulates one cooling-unit
// controller connecting to the server's ingest port and streaming SAMPLE
// frames at a fixed rate, for a fixed duration. run_benchmark.py launches
// many of these as separate processes to approximate N independent devices
// talking to the server at once.
//
// --rate-hz defaults to 10, not to "as fast as possible": 10 Hz is
// server/device_session.cpp's own kMaxSampleRateHz, the hard ceiling the
// server ever grants any device in its CONFIG reply. A real device,
// however fast it asked to go in HELLO, can never legally sample faster
// than that -- so pacing at 10 Hz is the fastest load this tool can send
// that still corresponds to something the real protocol allows. --rate-hz 0
// restores the original unlimited blast (still exactly what
// tests/integration/load_generator.cpp does): useful for deliberately
// reproducing the runaway-memory-growth failure mode this project hit the
// first time this tool was pointed at the server without any pacing at
// all -- a single connection ignoring backpressure can grow a subscriber's
// write buffer without bound, since nothing here reads the SLOW_DOWN
// frames the server sends in response. A real device (node/tasks/
// command_task.cpp) does read and obey them; this tool deliberately does
// not, which is exactly why it needs its own rate limit instead of relying
// on the server's.
//
// This is the same shape as tests/integration/load_generator.cpp (connect,
// send one HELLO, then send SAMPLE frames), generalized as docs/PLAN.md's
// stage 6 description says it eventually would be: a --device-id flag so
// many instances do not collide, a real clock in timestamp_ms instead of a
// fake per-frame counter, and --rate-hz pacing.
//
// timestamp_ms carries this process's own steady_clock reading, truncated
// to milliseconds. bench/latency_subscriber.cpp reads that same field back
// out of what the server forwards (server/store.cpp's record() passes the
// device's original timestamp_ms straight through to subscribers) and
// subtracts its own steady_clock reading to get ingest-to-delivery latency.
// This only works because both this process and the subscriber run on the
// same Linux host, where std::chrono::steady_clock is backed by
// CLOCK_MONOTONIC -- one clock shared by the whole machine, not a private
// clock per process. Across different machines this comparison would be
// meaningless.

#include "proto/frame_encoder.hpp"
#include "proto/messages.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
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

std::uint32_t now_ms() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ticks).count());
}

// Blocks the calling thread until `deadline`, using ::nanosleep instead of
// std::this_thread::sleep_until so this file does not have to link a
// threads library just to pace a single-threaded loop.
void sleep_until(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return;
    }

    const auto remaining_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(remaining_ns.count() / 1'000'000'000);
    ts.tv_nsec = static_cast<long>(remaining_ns.count() % 1'000'000'000);
    ::nanosleep(&ts, nullptr);
}

// Returns how many SAMPLE frames were actually handed to the kernel --
// run_benchmark.py sums this across every generator instance as the
// "offered load" side of its frames/sec and drop-count numbers.
// rate_hz == 0 means unlimited (send as fast as the loop runs); otherwise
// one frame is sent roughly every 1/rate_hz seconds.
std::uint64_t run_load(int fd, std::uint32_t device_id, std::uint8_t metric_id,
                        std::chrono::seconds duration, std::uint32_t rate_hz) {
    proto::HelloPayload hello{};
    hello.device_id = device_id;
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

    std::uint64_t sent = 0;
    std::int32_t value_milli = 0;
    const auto deadline = std::chrono::steady_clock::now() + duration;
    const auto interval = (rate_hz > 0)
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / rate_hz))
        : std::chrono::steady_clock::duration::zero();
    auto next_send = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        if (rate_hz > 0) {
            sleep_until(next_send);
            next_send += interval;
        }

        proto::SamplePayload sample{};
        sample.session_id = 0;
        sample.timestamp_ms = now_ms();
        sample.value_milli = value_milli;
        sample.metric_id = metric_id;

        std::uint8_t frame[proto::max_encoded_frame_size(sizeof(sample))];
        const proto::EncodeResult result = proto::encode_frame(
            proto::MessageType::kSample, reinterpret_cast<const std::uint8_t*>(&sample),
            sizeof(sample), frame, sizeof(frame));
        if (result.status == proto::EncodeStatus::kOk &&
            ::send(fd, frame, result.size, MSG_NOSIGNAL) == static_cast<ssize_t>(result.size)) {
            ++sent;
        }

        ++value_milli;
    }

    return sent;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6000;
    std::uint32_t device_id = 999;
    std::uint8_t metric_id = 1;
    std::chrono::seconds duration{5};
    std::uint32_t rate_hz = 10;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) {
            const std::string endpoint = argv[++i];
            const std::size_t colon = endpoint.find(':');
            if (colon != std::string::npos) {
                host = endpoint.substr(0, colon);
                port = static_cast<std::uint16_t>(std::atoi(endpoint.substr(colon + 1).c_str()));
            }
        } else if (arg == "--device-id" && i + 1 < argc) {
            device_id = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--metric" && i + 1 < argc) {
            metric_id = static_cast<std::uint8_t>(std::atoi(argv[++i]));
        } else if (arg == "--seconds" && i + 1 < argc) {
            duration = std::chrono::seconds(std::atoi(argv[++i]));
        } else if (arg == "--rate-hz" && i + 1 < argc) {
            rate_hz = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    const int fd = connect_to_server(host, port);
    if (fd < 0) {
        std::cerr << "sensorlink-bench-generator: failed to connect to " << host << ":" << port << "\n";
        return 1;
    }

    const std::uint64_t sent = run_load(fd, device_id, metric_id, duration, rate_hz);
    ::close(fd);

    // run_benchmark.py greps every generator's stdout for this line and sums
    // the counts -- see the comment on run_load() above.
    std::cout << "SENT " << sent << "\n";
    return 0;
}
