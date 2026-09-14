#include "server/store.hpp"

#include <algorithm>

namespace server {

void store::add_subscriber(subscriber* sub) {
    subscribers_.push_back(sub);
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
                    std::int32_t value_milli, std::uint32_t timestamp_ms) {
    device_history& h = history_[make_key(session_id, metric_id)];
    h.entries[h.next] = sample{timestamp_ms, value_milli};
    h.next = (h.next + 1) % kHistoryPerDevice;
    if (h.count < kHistoryPerDevice) {
        ++h.count;
    }

    const std::string name = metric_name(metric_id);
    for (subscriber* sub : subscribers_) {
        sub->publish(name, value_milli, timestamp_ms);
    }
}

}  // namespace server
