#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace client {

// The client-side twin of server/subscriber.cpp's own line-splitting loop:
// same problem (a TCP byte stream makes no promise about how bytes are
// grouped into recv() calls -- one line can arrive split in half, or two
// lines can arrive glued together), same shape, opposite end of the same
// socket. Deliberately free of sockets, so it can be driven directly with
// an in-memory buffer in tests, the same way server/subscriber.cpp's own
// line-splitting is tested without a real network connection.
//
// Scans buffer for every complete line it holds (ending in '\n', tolerating
// a preceding '\r'), and for each one:
//   - tries to parse it as "<metric> <timestamp_ms> <value_milli>" (the
//     exact format server/subscriber.cpp's publish() sends -- three
//     whitespace-separated fields, the last two parsing fully as
//     integers); if it parses, prints it and increments valid_count.
//   - otherwise, prints it as-is and increments malformed_count. This
//     project's text protocol carries no CRC, so a line that does not
//     parse is this client's equivalent of a corrupted frame -- this is
//     what stands in for the "CRC-error counter" in docs/PLAN.md's stage 4
//     description, adapted to a protocol that has no CRC to check.
// Removes every consumed line from the front of buffer; a trailing partial
// line's bytes are left in place, to be completed by a later read.
void handle_incoming(std::vector<std::uint8_t>& buffer, std::size_t& valid_count, std::size_t& malformed_count);

}  // namespace client
