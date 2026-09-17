#include "server/store.hpp"

#include <algorithm>

namespace server {
namespace {

// Above this, a pharmacy fridge (this project's running example) is out of
// spec -- moved verbatim from what used to be node/tasks/process_task.cpp.
constexpr std::int32_t kHighThresholdMilli = 5500;  // 5.5 degrees C

// Matches metric_name()'s case 0 below. The excursion threshold only means
// something for temperature -- this project defines no threshold for any
// other metric today, so the check in record() is scoped to this one id
// rather than a speculative per-metric threshold table.
constexpr std::uint8_t kTempMetricId = 0;

}  // namespace

void store::add_subscriber(subscriber* sub) {
    subscribers_.push_back(sub);

    // Replay every alert that is already active -- see the doc comment on
    // this function in store.hpp for why this is required, not optional.
    for (const auto& [session_id, alerts] : alerts_) {
        if (alerts.excursion.active) {
            sub->publish_excursion_alert(session_id, alerts.excursion.value_milli,
                                          alerts.excursion.timestamp_ms);
        }
        if (alerts.offline.active) {
            sub->publish_offline_alert(session_id, alerts.offline.timestamp_ms);
        }
    }
}

void store::remove_subscriber(subscriber* sub) {
    subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), sub),
                        subscribers_.end());
}

std::uint32_t store::make_key(std::uint16_t session_id, std::uint8_t metric_id) {
    // metric_id is a full byte (0-255): shifting session_id left by exactly
    // 8 bits leaves precisely enough room in the low byte for metric_id to
    // sit without colliding with it -- the same "how many bits does this
    // value need" reasoning already used for metric_mask in
    // device_session.cpp.
    return (static_cast<std::uint32_t>(session_id) << 8) | metric_id;
}

std::string store::metric_name(std::uint8_t metric_id) {
    switch (metric_id) {
        case 0:
            return "temp";
        case 1:
            return "pressure";
        default:
            return "metric" + std::to_string(metric_id);
    }
}

std::vector<store::sample> store::history(std::uint16_t session_id, std::uint8_t metric_id) const {
    const auto it = history_.find(make_key(session_id, metric_id));
    if (it == history_.end()) {
        return {};
    }

    const device_history& h = it->second;
    std::vector<sample> result;
    result.reserve(h.count);

    // Once the buffer has wrapped (count == capacity), entries[next] holds
    // the oldest surviving sample -- next is exactly the slot record() is
    // about to overwrite, i.e. the one written longest ago. Before it has
    // wrapped, nothing has been overwritten yet and the oldest sample is
    // simply at index 0.
    const std::size_t oldest = (h.count < kHistoryPerDevice) ? 0 : h.next;
    for (std::size_t i = 0; i < h.count; ++i) {
        result.push_back(h.entries[(oldest + i) % kHistoryPerDevice]);
    }
    return result;
}

void store::record(std::uint16_t session_id, std::uint8_t metric_id,
                    std::int32_t value_milli, std::uint32_t timestamp_ms,
                    std::chrono::steady_clock::time_point now) {
    device_history& h = history_[make_key(session_id, metric_id)];
    h.entries[h.next] = sample{timestamp_ms, value_milli};
    h.next = (h.next + 1) % kHistoryPerDevice;
    if (h.count < kHistoryPerDevice) {
        ++h.count;
    }

    // Exponential moving average, moved verbatim from what used to be
    // node/tasks/process_task.cpp: each new sample nudges the average a
    // quarter of the way toward it (">> 2" is "/ 4" without a division
    // instruction). Per (session, metric) instead of per-device, since
    // device_history is already keyed that way -- a pressure stream gets
    // its own independent average at no extra cost.
    if (!h.have_average) {
        h.average_milli = value_milli;
        h.have_average = true;
    } else {
        h.average_milli += (value_milli - h.average_milli) >> 2;
    }

    const std::string name = metric_name(metric_id);
    for (subscriber* sub : subscribers_) {
        sub->publish(name, session_id, value_milli, timestamp_ms);
    }

    if (metric_id != kTempMetricId || h.average_milli <= kHighThresholdMilli) {
        return;
    }

    device_alerts& alerts = alerts_[session_id];
    if (alerts.excursion.active) {
        return;  // already latched -- no re-evaluation until ack()
    }
    alerts.excursion = alert_state{/*active=*/true, h.average_milli, timestamp_ms, now};
    notify_excursion(session_id, alerts.excursion);
}

void store::latch_offline(std::uint16_t session_id, std::uint32_t timestamp_ms,
                           std::chrono::steady_clock::time_point now) {
    device_alerts& alerts = alerts_[session_id];
    if (alerts.offline.active) {
        return;
    }
    alerts.offline = alert_state{/*active=*/true, /*value_milli=*/0, timestamp_ms, now};
    notify_offline(session_id, alerts.offline);
}

store::alert_state store::excursion_alert(std::uint16_t session_id) const {
    const auto it = alerts_.find(session_id);
    return it == alerts_.end() ? alert_state{} : it->second.excursion;
}

store::alert_state store::offline_alert(std::uint16_t session_id) const {
    const auto it = alerts_.find(session_id);
    return it == alerts_.end() ? alert_state{} : it->second.offline;
}

void store::notify_excursion(std::uint16_t session_id, const alert_state& a) {
    for (subscriber* sub : subscribers_) {
        sub->publish_excursion_alert(session_id, a.value_milli, a.timestamp_ms);
    }
}

void store::notify_offline(std::uint16_t session_id, const alert_state& a) {
    for (subscriber* sub : subscribers_) {
        sub->publish_offline_alert(session_id, a.timestamp_ms);
    }
}

store::ack_result store::ack(std::uint16_t session_id, std::chrono::steady_clock::time_point now) {
    ack_result result;
    const auto it = alerts_.find(session_id);
    if (it == alerts_.end()) {
        return result;
    }
    device_alerts& alerts = it->second;

    if (alerts.offline.active) {
        result.had_offline = true;
        result.offline_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - alerts.offline.triggered_at);
        alerts.offline.active = false;  // OFFLINE is always final -- see the class-level contract
    }

    if (alerts.excursion.active) {
        result.had_excursion = true;
        result.excursion_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - alerts.excursion.triggered_at);

        const auto hist_it = history_.find(make_key(session_id, kTempMetricId));
        const bool recovered =
            hist_it != history_.end() && hist_it->second.average_milli <= kHighThresholdMilli;
        if (recovered) {
            alerts.excursion.active = false;
        } else {
            result.excursion_still_active = true;
        }
    }

    return result;
}
}  // namespace server
