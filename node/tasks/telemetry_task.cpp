// The last stage of the outgoing pipeline: turns a processed reading into
// an actual wire-format proto::SamplePayload, encodes it with the exact
// same proto::frame_encoder that 57 host-side tests exercised in stage 1,
// and writes the resulting bytes out over the UART. This is the one place
// in the whole node/ tree that has any idea proto:: exists -- everything
// upstream (sensor_task, process_task) works in plain internal structs and
// has never heard of a session_id or a CRC.
#include "tasks/telemetry_task.hpp"

#include "proto/frame_encoder.hpp"
#include "proto/messages.hpp"

#include "drivers/uart_cmsdk.hpp"
#include "os/Mutex.hpp"
#include "shared_state.hpp"

namespace tasks {

void TelemetryTask(void* context) {
    auto* pipeline = static_cast<Pipeline*>(context);
    ProcessedReading reading{};

    for (;;) {
        if (!pipeline->processed_queue.receive(reading)) {
            continue;
        }

        std::uint16_t session_id;
        {
            os::Mutex::Guard lock(pipeline->config_mutex);
            session_id = pipeline->session.session_id;
        }

        proto::SamplePayload payload{};
        payload.session_id = session_id;
        payload.timestamp_ms = reading.timestamp_ms;
        payload.value_milli = reading.value_milli;
        payload.metric_id = 0;

        std::uint8_t frame[proto::max_encoded_frame_size(sizeof(payload))];
        const proto::EncodeResult result =
            proto::encode_frame(proto::MessageType::kSample,
                                 reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload),
                                 frame, sizeof(frame));

        if (result.status == proto::EncodeStatus::kOk) {
            pipeline->uart->write(frame, result.size);
        }
    }
}

}  // namespace tasks
