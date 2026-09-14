#include "server/device_session.hpp"

#include <algorithm>

namespace server {

device_session::device_session(std::uint16_t session_id, std::chrono::steady_clock::time_point now)
    : session_id_(session_id), last_activity_(now) {}

void device_session::note_activity(std::chrono::steady_clock::time_point now) {
    last_activity_ = now;
}

bool device_session::timed_out(std::chrono::steady_clock::time_point now,
                                std::chrono::milliseconds timeout) const {
    return (now - last_activity_) > timeout;
}

proto::SlowDownPayload device_session::slow_down(std::uint8_t new_rate_hz) {
    current_rate_hz_ = new_rate_hz;
    return proto::SlowDownPayload{session_id_, new_rate_hz};
}

namespace {
constexpr std::uint8_t kMaxSampleRateHz = 10;
}  // namespace

proto::ConfigPayload device_session::handle_hello(const proto::HelloPayload& hello) {
    connected_ = true;
    current_rate_hz_ = std::min(hello.offered_rate_hz, kMaxSampleRateHz);

    std::uint8_t metric_mask = 0;
    const std::uint8_t usable_metrics = std::min(hello.metric_count, static_cast<std::uint8_t>(8));
    for (std::uint8_t i = 0; i < usable_metrics; ++i) {
        metric_mask = static_cast<std::uint8_t>(metric_mask | (1u << i));
    }

    return proto::ConfigPayload{session_id_, current_rate_hz_, metric_mask};
}

}  // namespace server
