#include "session.hpp"

#include "FreeRTOS.h"
#include "task.h"

#include "proto/frame_encoder.hpp"
#include "proto/messages.hpp"

#include "drivers/uart_cmsdk.hpp"
#include "shared_state.hpp"

namespace tasks {
namespace {

constexpr std::uint32_t kDeviceId = 1;     // one fixed fake device id for this demo
constexpr std::uint8_t kMetricCount = 1;   // temperature only

// Encodes payload as message type `type` and writes the resulting frame to
// pipeline->uart. Shared by SendHello and SendHeartbeat so the encode/write
// mechanics live in exactly one place.
template <typename Payload>
void SendMessage(Pipeline& pipeline, proto::MessageType type, const Payload& payload) {
    std::uint8_t frame[proto::max_encoded_frame_size(sizeof(payload))];
    const proto::EncodeResult result =
        proto::encode_frame(type, reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload),
                             frame, sizeof(frame));
    if (result.status == proto::EncodeStatus::kOk) {
        pipeline.uart->write(frame, result.size);
    }
}

}  // namespace

void SendHello(Pipeline& pipeline) {
    proto::HelloPayload payload{};
    payload.device_id = kDeviceId;
    payload.fw_version = proto::kProtocolVersion;
    payload.metric_count = kMetricCount;
    payload.offered_rate_hz = pipeline.config.sample_rate_hz;
    SendMessage(pipeline, proto::MessageType::kHello, payload);
}

void SendHeartbeat(Pipeline& pipeline) {
    proto::HeartbeatPayload payload{};
    payload.session_id = pipeline.session.session_id;
    payload.uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    SendMessage(pipeline, proto::MessageType::kHeartbeat, payload);
}

}  // namespace tasks
