#pragma once

#include "server/subscriber.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace server {

// The central in-memory data hub: keeps a short, bounded history of recent
// readings per device, computes each device's smoothed reading and decides
// whether it is out of range, and fans every new reading -- and every
// alert -- out to whichever subscribers are currently connected. This is
// "a recent-history ring buffer per device in memory (no database)" plus
// "a live feed of any device's data" from the project overview, in one
// object.
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

    // One latched alert: either a temperature excursion or a device gone
    // quiet. `active` is the only field that ever needs a fresh look from
    // the outside; the rest is context for whoever is deciding what to do
    // about it.
    struct alert_state {
        bool active = false;
        std::int32_t value_milli = 0;  // the smoothed reading that triggered it; 0 for OFFLINE
        std::uint32_t timestamp_ms = 0;  // the device's own clock, for correlating with its logs

        // The server's own clock, taken when this alert last became active.
        // Never sent over the wire -- see ack()'s contract for why it exists
        // and, critically, when it is and is not allowed to move.
        std::chrono::steady_clock::time_point triggered_at{};
    };

    // What ack() reports back about the device it just acknowledged: how
    // long each kind of alert (if any) had been open, in the server's own
    // time. See ack()'s doc comment for what excursion_still_active means.
    struct ack_result {
        bool had_excursion = false;
        std::chrono::milliseconds excursion_duration{0};
        bool excursion_still_active = false;
        bool had_offline = false;
        std::chrono::milliseconds offline_duration{0};
    };

    // Registers sub so it receives every future record() call for whichever
    // metric it is subscribed to at the time, and every alert regardless of
    // subscription (see publish_excursion_alert/publish_offline_alert).
    // Also immediately replays every alert that is already active to sub
    // alone -- otherwise a dashboard that connects after an alert has
    // already latched would never learn about it, since a latched alert by
    // definition does not repeat itself. Does not take ownership -- the
    // caller must call remove_subscriber before destroying sub.
    void add_subscriber(subscriber* sub);
    void remove_subscriber(subscriber* sub);

    // Called by ingest for every SAMPLE frame that decodes. Appends to that
    // device's history, updates its smoothed (EMA) reading for this metric,
    // and pushes the reading out to every currently-matching subscriber. For
    // the temperature metric specifically, also checks the smoothed reading
    // against the excursion threshold and latches a new alert if it just
    // crossed it and none is already open. now is the server's own clock,
    // used only to stamp a newly-latched alert's triggered_at -- never
    // compared against timestamp_ms, which is the device's own clock.
    void record(std::uint16_t session_id, std::uint8_t metric_id,
                std::int32_t value_milli, std::uint32_t timestamp_ms,
                std::chrono::steady_clock::time_point now);

    // Latches an OFFLINE alert for session_id, unless one is already active
    // (idempotent -- main.cpp's housekeeping calls this once per eviction,
    // but nothing here relies on that). timestamp_ms is the device's own
    // last-known clock (device_session::last_uptime_ms()), for the same
    // reason record()'s is. now is the server's clock, for triggered_at.
    void latch_offline(std::uint16_t session_id, std::uint32_t timestamp_ms,
                        std::chrono::steady_clock::time_point now);

    // Acknowledges session_id's currently active alerts (both kinds, if
    // both are open -- see the ACK protocol's "one device at a time" design
    // note in subscriber.cpp). now is the server's clock.
    //
    // For OFFLINE: session_id never reconnects under the same id (see the
    // "known limitation" note in server/main.cpp), so acknowledging it is
    // always final -- it simply closes.
    //
    // For an excursion: closing it is only safe if the condition that
    // caused it has actually gone away. ack() re-checks the device's
    // current smoothed reading at the moment it is called:
    //   - if it has dropped back below the threshold, the alert closes for
    //     real, and a future crossing latches a brand new one with its own
    //     triggered_at.
    //   - if it is still above the threshold, the alert is NOT closed --
    //     triggered_at is left untouched, and excursion_still_active comes
    //     back true. This is deliberate: without it, acknowledging an
    //     ongoing excursion over and over (each one immediately re-latching
    //     on the next sample) would report a short, misleadingly small
    //     duration every time, hiding how long the device has actually been
    //     out of range.
    // Either way, excursion_duration is always now - triggered_at, computed
    // BEFORE any of the above -- an operator calling ack() on an alert that
    // is still open gets an accurate "how long so far", not a stale number.
    ack_result ack(std::uint16_t session_id, std::chrono::steady_clock::time_point now);

    alert_state excursion_alert(std::uint16_t session_id) const;
    alert_state offline_alert(std::uint16_t session_id) const;

    // Up to kHistoryPerDevice most recent samples for one device's metric,
    // oldest first. Empty if nothing has been recorded for it yet.
    std::vector<sample> history(std::uint16_t session_id, std::uint8_t metric_id) const;

private:
    struct device_history {
        sample entries[kHistoryPerDevice]{};
        std::size_t count = 0;
        std::size_t next = 0;  // index the next record() call will write to

        // The device's smoothed reading for this metric -- moved here
        // verbatim from what used to be node/tasks/process_task.cpp's local
        // variables. have_average is false only before this (device,
        // metric) pair's very first sample.
        bool have_average = false;
        std::int32_t average_milli = 0;
    };

    // One device's alert state, of both kinds. A single ACK clears both at
    // once (see subscriber.cpp) -- a technician inspects one physical
    // fridge, not one sensor inside it.
    struct device_alerts {
        alert_state excursion;
        alert_state offline;
    };

    static std::uint32_t make_key(std::uint16_t session_id, std::uint8_t metric_id);
    static std::string metric_name(std::uint8_t metric_id);

    void notify_excursion(std::uint16_t session_id, const alert_state& a);
    void notify_offline(std::uint16_t session_id, const alert_state& a);

    std::unordered_map<std::uint32_t, device_history> history_;
    std::unordered_map<std::uint16_t, device_alerts> alerts_;
    std::vector<subscriber*> subscribers_;
};

}  // namespace server
