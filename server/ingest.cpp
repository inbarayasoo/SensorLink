#include "server/ingest.hpp"

#include "proto/frame_encoder.hpp"

#include <chrono>
#include <cstring>

namespace server {

ingest::ingest(connection& conn, std::uint16_t session_id, store& data_store)
    : connection_(conn), session_id_(session_id), store_(data_store) {}

void ingest::on_readable() {
    const std::vector<std::uint8_t>& bytes = connection_.read_buffer();
    if (bytes.empty()) {
        return;
    }

    parser_.push_bytes(bytes.data(), bytes.size(),
                        [this](const proto::ParsedFrame& frame) { dispatch(frame); });

    // Every byte handed to push_bytes is now either part of a completed
    // frame or sitting in the parser's own internal buffer, waiting for the
    // rest of a still-incomplete one -- connection's copy of it is no
    // longer needed either way.
    connection_.consume(bytes.size());
}

void ingest::dispatch(const proto::ParsedFrame& frame) {
    switch (frame.type) {
        case proto::MessageType::kHello:
            if (frame.payload_size == sizeof(proto::HelloPayload)) {
                proto::HelloPayload payload;
                std::memcpy(&payload, frame.payload, sizeof(payload));
                handle_hello(payload);
            }
            break;
        case proto::MessageType::kHeartbeat:
            if (frame.payload_size == sizeof(proto::HeartbeatPayload)) {
                proto::HeartbeatPayload payload;
                std::memcpy(&payload, frame.payload, sizeof(payload));
                handle_heartbeat(payload);
            }
            break;
        case proto::MessageType::kSample:
            if (frame.payload_size == sizeof(proto::SamplePayload)) {
                proto::SamplePayload payload;
                std::memcpy(&payload, frame.payload, sizeof(payload));
                handle_sample(payload);
            }
            break;
        default:
            break;
    }
}

void ingest::handle_heartbeat(const proto::HeartbeatPayload&) {
    if (session_) {
        session_->note_activity(std::chrono::steady_clock::now());
    }
}

void ingest::handle_sample(const proto::SamplePayload& payload) {
    if (!session_) {
        return;
    }
    session_->note_activity(std::chrono::steady_clock::now());
    store_.record(session_->session_id(), payload.metric_id, payload.value_milli, payload.timestamp_ms);
}

void ingest::handle_hello(const proto::HelloPayload& payload) {
    session_.emplace(session_id_, std::chrono::steady_clock::now());
    const proto::ConfigPayload config = session_->handle_hello(payload);
    send_frame(proto::MessageType::kConfig,
               reinterpret_cast<const std::uint8_t*>(&config), sizeof(config));
}

void ingest::send_slow_down(std::uint8_t new_rate_hz) {
    if (!session_) {
        return;
    }
    const proto::SlowDownPayload payload = session_->slow_down(new_rate_hz);
    send_frame(proto::MessageType::kSlowDown,
               reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload));
}

void ingest::send_frame(proto::MessageType type, const std::uint8_t* payload, std::size_t payload_size) {
    std::uint8_t out[proto::max_encoded_frame_size(proto::kMaxPayloadSize)];
    const proto::EncodeResult result = proto::encode_frame(type, payload, payload_size, out, sizeof(out));
    if (result.status == proto::EncodeStatus::kOk) {
        connection_.enqueue_write(out, result.size);
    }
}

}  // namespace server
