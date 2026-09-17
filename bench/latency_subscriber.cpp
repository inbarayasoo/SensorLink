// The measurement side of bench/run_benchmark.py's load test: connects to
// the server's subscribe port exactly like client/ does, subscribes to one
// metric, and for every update line that arrives, computes how long it took
// to get from bench/load_generator.cpp's send() call to here.
//
// server/subscriber.cpp's publish() sends "<metric> <session_id>
// <timestamp_ms> <value_milli>\n" -- the same line format
// client/line_protocol.cpp parses.
// timestamp_ms here is the generator's own steady_clock reading at send
// time (see bench/load_generator.cpp's comment on why that clock is safe to
// compare across processes on the same host); subtracting it from this
// process's own steady_clock reading, taken the instant the line is parsed,
// gives the ingest-to-delivery latency in milliseconds.
//
// Runs for a fixed duration, then prints:
//   COUNT <n>
//   P50_MS <value>
//   P99_MS <value>
// for run_benchmark.py to parse out of its stdout.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>

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

    // Without this, recv() blocks indefinitely once the load generators stop
    // sending -- there would be nothing left to wake it up, and the process
    // would hang past its own --seconds duration instead of printing results
    // and exiting. A 200ms timeout just makes the read loop below re-check
    // its deadline regularly; it is not a measurement of anything.
    timeval timeout{};
    timeout.tv_usec = 200'000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    return fd;
}

std::int64_t now_ms() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ticks).count());
}

void process_lines(std::vector<std::uint8_t>& buffer, const std::string& metric,
                    std::vector<std::int64_t>& latencies_ms, std::uint64_t& count) {
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] != '\n') {
            continue;
        }

        std::size_t line_end = i;
        if (line_end > line_start && buffer[line_end - 1] == '\r') {
            --line_end;
        }

        const std::string line(buffer.begin() + static_cast<long>(line_start),
                                buffer.begin() + static_cast<long>(line_end));

        const std::size_t first_space = line.find(' ');
        if (first_space != std::string::npos && line.substr(0, first_space) == metric) {
            // Skip the session_id field (2nd) to reach timestamp_ms (3rd) --
            // see the header comment on the current wire format.
            const std::size_t second_space = line.find(' ', first_space + 1);
            const std::size_t third_space =
                (second_space == std::string::npos) ? std::string::npos : line.find(' ', second_space + 1);
            if (second_space == std::string::npos) {
                line_start = i + 1;
                continue;
            }
            const std::string ts_field = (third_space == std::string::npos)
                ? line.substr(second_space + 1)
                : line.substr(second_space + 1, third_space - second_space - 1);
            try {
                const std::int64_t timestamp_ms = std::stoll(ts_field);
                latencies_ms.push_back(now_ms() - timestamp_ms);
                ++count;
            } catch (const std::exception&) {
                // malformed field -- skip this line, not worth aborting a run over
            }
        }

        line_start = i + 1;
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(line_start));
}

std::int64_t percentile(std::vector<std::int64_t> latencies_ms, double p) {
    std::sort(latencies_ms.begin(), latencies_ms.end());
    const std::size_t n = latencies_ms.size();
    const std::size_t rank = static_cast<std::size_t>(
        std::llround(p / 100.0 * static_cast<double>(n - 1)));
    return latencies_ms[rank];
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;
    std::string metric = "pressure";
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
            metric = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            duration = std::chrono::seconds(std::atoi(argv[++i]));
        }
    }

    const int fd = connect_to_server(host, port);
    if (fd < 0) {
        std::cerr << "sensorlink-bench-subscriber: failed to connect to " << host << ":" << port << "\n";
        return 1;
    }

    const std::string subscribe_line = "SUBSCRIBE " + metric + "\n";
    ::send(fd, subscribe_line.data(), subscribe_line.size(), MSG_NOSIGNAL);

    std::vector<std::uint8_t> buffer;
    std::vector<std::int64_t> latencies_ms;
    std::uint64_t count = 0;

    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint8_t chunk[4096];
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received < 0) {
            continue;
        }
        if (received == 0) {
            break;  // server closed the connection
        }
        buffer.insert(buffer.end(), chunk, chunk + received);
        process_lines(buffer, metric, latencies_ms, count);
    }

    ::close(fd);

    std::cout << "COUNT " << count << "\n";
    if (count > 0) {
        std::cout << "P50_MS " << percentile(latencies_ms, 50.0) << "\n";
        std::cout << "P99_MS " << percentile(latencies_ms, 99.0) << "\n";
    }
    return 0;
}
