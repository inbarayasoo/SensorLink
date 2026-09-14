// The other end of the text protocol server/subscriber.cpp speaks: a
// stand-in for the cold-chain QA dashboard itself. Connects to the
// server's subscriber port, sends one SUBSCRIBE, and prints every reading
// as it arrives. In a real control room this would be a GUI; here it is
// the minimum that proves the live feed actually reaches a client outside
// the server process.
//
// One connection, one socket, nothing else running at the same time -- so
// unlike server/main.cpp, this is plain blocking I/O: connect() and
// recv() are allowed to block, because there is nothing else for this
// process to be doing while they do.

#include "client/line_protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace {

// --- for you to complete -------------------------------------------------
//
// Resolves host (a literal IPv4 address, e.g. "127.0.0.1" -- no DNS lookup
// needed for this project) and connects a plain, blocking TCP socket to
// host:port. Returns the connected fd, or -1 if any step failed.
//
//   socket()     -- AF_INET, SOCK_STREAM. No SOCK_NONBLOCK this time: a
//                   single-connection client has nothing else to do while
//                   connect() or recv() block, unlike the server.
//   inet_pton()  -- parses the dotted-decimal string into the binary
//                   address sockaddr_in expects -- the reverse of what
//                   the server's own listener needed, since it just bound
//                   to INADDR_ANY instead of a specific address.
//   connect()    -- to host:port.
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
// ---------------------------------------------------------------------------

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6001;
    std::string metric = "temp";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) {
            const std::string endpoint = argv[++i];
            const std::size_t colon = endpoint.find(':');
            if (colon != std::string::npos) {
                host = endpoint.substr(0, colon);
                port = static_cast<std::uint16_t>(std::atoi(endpoint.substr(colon + 1).c_str()));
            }
        } else if (arg == "--subscribe" && i + 1 < argc) {
            metric = argv[++i];
        }
    }

    const int fd = connect_to_server(host, port);
    if (fd < 0) {
        std::cerr << "sensorlink-client: failed to connect to " << host << ":" << port << "\n";
        return 1;
    }

    const std::string subscribe_line = "SUBSCRIBE " + metric + "\n";
    if (::send(fd, subscribe_line.data(), subscribe_line.size(), MSG_NOSIGNAL) < 0) {
        std::cerr << "sensorlink-client: failed to send SUBSCRIBE\n";
        ::close(fd);
        return 1;
    }

    std::cout << "subscribed to " << metric << " -- waiting for updates" << std::endl;

    std::vector<std::uint8_t> buffer;
    std::size_t valid_count = 0;
    std::size_t malformed_count = 0;

    while (true) {
        std::uint8_t chunk[4096];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;  // the server closed the connection, or a fatal error -- either way, stop
        }
        buffer.insert(buffer.end(), chunk, chunk + n);
        client::handle_incoming(buffer, valid_count, malformed_count);
    }

    std::cout << "connection closed -- " << valid_count << " valid update(s), "
              << malformed_count << " malformed line(s)\n";
    ::close(fd);
}
