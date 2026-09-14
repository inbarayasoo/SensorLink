#pragma once

#include "server/subscriber.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace server {

// The central in-memory data hub: keeps a short, bounded history of recent
// readings per device, and fans every new reading out to whichever
// subscribers are currently watching that metric. This is "a recent-history
// ring buffer per device in memory (no database)" plus "a live feed of any
// device's data" from the project overview, in one object.
//
// One store exists for the whole server -- every device's samples and
// every subscriber's live feed all pass through it.
class store {
public:
    // How many of the most recent samples are kept per (device, metric).
    // At the server's own sample-rate cap of 10 Hz (see device_session.cpp),
    // 64 entries covers roughly the last 6 seconds of readings at the
    // fastest a device is ever allowed to send -- and several minutes at
    // the slower rates actually exercised in stage 2. That is enough for a
    // live dashboard's short recent trend, at a small, fixed per-device
    // memory cost no matter how many devices are connected.
    static constexpr std::size_t kHistoryPerDevice = 64;

    struct sample {
        std::uint32_t timestamp_ms;
        std::int32_t value_milli;
    };

    // Registers sub so it receives every future record() call for whichever
    // metric it is subscribed to at the time. Does not take ownership -- the
    // caller must call remove_subscriber before destroying sub.
    void add_subscriber(subscriber* sub);
    void remove_subscriber(subscriber* sub);

    // Called by ingest for every SAMPLE frame that decodes. Appends to that
    // device's history (overwriting the oldest entry once full) and pushes
    // the reading out to every currently-matching subscriber.
    void record(std::uint16_t session_id, std::uint8_t metric_id,
                std::int32_t value_milli, std::uint32_t timestamp_ms);

    // Up to kHistoryPerDevice most recent samples for one device's metric,
    // oldest first. Empty if nothing has been recorded for it yet.
    std::vector<sample> history(std::uint16_t session_id, std::uint8_t metric_id) const;

private:
    struct device_history {
        sample entries[kHistoryPerDevice]{};
        std::size_t count = 0;
        std::size_t next = 0;  // index the next record() call will write to
    };

    static std::uint32_t make_key(std::uint16_t session_id, std::uint8_t metric_id);
    static std::string metric_name(std::uint8_t metric_id);

    std::unordered_map<std::uint32_t, device_history> history_;
    std::vector<subscriber*> subscribers_;
};

}  // namespace server
