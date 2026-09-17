#include "server/ingest.hpp"

#include "proto/frame_encoder.hpp"
#include "proto/frame_parser.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Same socketpair fixture as connection_test.cpp: fds_[0] is owned by the
// connection under test, fds_[1] plays the wire's other end (the device, in
// this file's tests) so the test can write raw device bytes and read raw
// server bytes without a real network.
class IngestTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds_), 0);
    }

    void TearDown() override {
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
        }
    }

    // Encodes one frame and writes it to fds_[1], as if a device sent it.
    template <typename Payload>
    void SendFromDevice(proto::MessageType type, const Payload& payload) {
        std::uint8_t encoded[proto::max_encoded_frame_size(sizeof(Payload))];
        const proto::EncodeResult result = proto::encode_frame(
            type, reinterpret_cast<const std::uint8_t*>(&payload), sizeof(Payload),
            encoded, sizeof(encoded));
        ASSERT_EQ(result.status, proto::EncodeStatus::kOk);
        ASSERT_EQ(::write(fds_[1], encoded, result.size), static_cast<ssize_t>(result.size));
    }

    // Reads whatever the server has written back so far and decodes exactly
    // one frame from it. Returns a zeroed frame (and records a failure) if
    // read() did not return actual bytes -- ASSERT_* cannot be used here
    // since this function does not return void, so a bad n is guarded by
    // hand instead of letting a negative read count reach static_cast<size_t>.
    proto::ParsedFrame ReadOneFrameFromServer() {
        std::uint8_t raw[256];
        const ssize_t n = ::read(fds_[1], raw, sizeof(raw));
        if (n <= 0) {
            ADD_FAILURE() << "expected the server to have written a reply; read() returned " << n;
            return proto::ParsedFrame{};
        }

        proto::frame_parser<> decoder;
        proto::ParsedFrame captured{};
        bool found = false;
        decoder.push_bytes(raw, static_cast<std::size_t>(n), [&](const proto::ParsedFrame& frame) {
            captured = frame;
            found = true;
        });
        EXPECT_TRUE(found);
        return captured;
    }

    int fds_[2] = {-1, -1};
};

}  // namespace

TEST_F(IngestTest, HelloProducesAMatchingConfigReplyOnTheWire) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/99, data_store);

    SendFromDevice(proto::MessageType::kHello,
                    proto::HelloPayload{/*device_id=*/1, /*fw_version=*/1,
                                         /*metric_count=*/1, /*offered_rate_hz=*/2});
    // Driven directly, the same way connection_test.cpp drives connection:
    // try_read() only fills the buffer, ing.on_readable() is what parses it
    // and enqueues the CONFIG reply, and try_write() flushes that reply to
    // the wire. Production wires these same three steps together through
    // connection's on_readable hook and the real epoll callback instead.
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();
    ASSERT_TRUE(conn.try_write());

    const proto::ParsedFrame reply = ReadOneFrameFromServer();
    ASSERT_EQ(reply.type, proto::MessageType::kConfig);
    ASSERT_EQ(reply.payload_size, sizeof(proto::ConfigPayload));

    proto::ConfigPayload config{};
    std::memcpy(&config, reply.payload, sizeof(config));
    EXPECT_EQ(config.session_id, 99);
    EXPECT_EQ(config.sample_rate_hz, 2);

    ASSERT_TRUE(ing.has_session());
    EXPECT_EQ(ing.session()->session_id(), 99);
}

TEST_F(IngestTest, TheOnReadableHookFiresAutomaticallyThroughTheRealEventLoop) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/7, data_store);
    conn.on_readable([&] { ing.on_readable(); });

    SendFromDevice(proto::MessageType::kHello,
                    proto::HelloPayload{/*device_id=*/1, /*fw_version=*/1,
                                         /*metric_count=*/1, /*offered_rate_hz=*/2});
    // First iteration: fds_[0] is EPOLLIN-ready, so the loop's own callback
    // calls try_read() then the registered on_readable hook -- no direct
    // calls into connection or ingest at all this time. That enqueues the
    // CONFIG reply and starts watching EPOLLOUT, but does not flush it yet.
    ASSERT_EQ(loop.poll(), 1);
    ASSERT_TRUE(ing.has_session());

    // Second iteration: fds_[0] is now EPOLLOUT-ready, so the loop's
    // callback calls try_write() and the CONFIG reply actually reaches the
    // wire.
    ASSERT_EQ(loop.poll(), 1);

    const proto::ParsedFrame reply = ReadOneFrameFromServer();
    EXPECT_EQ(reply.type, proto::MessageType::kConfig);
}

TEST_F(IngestTest, HeartbeatBeforeHelloIsIgnoredWithoutCrashing) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/1, data_store);

    SendFromDevice(proto::MessageType::kHeartbeat,
                    proto::HeartbeatPayload{/*session_id=*/0, /*uptime_ms=*/1000});
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();

    EXPECT_FALSE(ing.has_session());
}

TEST_F(IngestTest, SendSlowDownEncodesAFrameWithTheLoweredRate) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/42, data_store);

    SendFromDevice(proto::MessageType::kHello,
                    proto::HelloPayload{/*device_id=*/1, /*fw_version=*/1,
                                         /*metric_count=*/1, /*offered_rate_hz=*/10});
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();
    ASSERT_TRUE(conn.try_write());
    ReadOneFrameFromServer();  // drain the CONFIG reply so only SLOW_DOWN is left on the wire

    ing.send_slow_down(3);
    ASSERT_TRUE(conn.try_write());

    const proto::ParsedFrame reply = ReadOneFrameFromServer();
    ASSERT_EQ(reply.type, proto::MessageType::kSlowDown);
    ASSERT_EQ(reply.payload_size, sizeof(proto::SlowDownPayload));

    proto::SlowDownPayload payload{};
    std::memcpy(&payload, reply.payload, sizeof(payload));
    EXPECT_EQ(payload.session_id, 42);
    EXPECT_EQ(payload.new_rate_hz, 3);
    EXPECT_EQ(ing.session()->current_rate_hz(), 3);
}

TEST_F(IngestTest, SendSlowDownBeforeHelloIsIgnoredWithoutCrashing) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/1, data_store);

    ing.send_slow_down(1);  // no session yet -- must not crash or write anything

    EXPECT_FALSE(ing.has_session());
}

TEST_F(IngestTest, MalformedHelloPayloadSizeIsIgnored) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/1, data_store);

    // Three bytes is not a valid HelloPayload (8 bytes) -- this stands in
    // for a buggy or hostile device sending a HELLO with the wrong shape.
    const std::uint8_t bogus_payload[3] = {1, 2, 3};
    std::uint8_t encoded[proto::max_encoded_frame_size(sizeof(bogus_payload))];
    const proto::EncodeResult result = proto::encode_frame(
        proto::MessageType::kHello, bogus_payload, sizeof(bogus_payload), encoded, sizeof(encoded));
    ASSERT_EQ(result.status, proto::EncodeStatus::kOk);
    ASSERT_EQ(::write(fds_[1], encoded, result.size), static_cast<ssize_t>(result.size));

    ASSERT_TRUE(conn.try_read());
    ing.on_readable();

    EXPECT_FALSE(ing.has_session());
}

TEST_F(IngestTest, SampleUpdatesTheSessionsLastUptimeMs) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/1, data_store);

    SendFromDevice(proto::MessageType::kHello,
                    proto::HelloPayload{/*device_id=*/1, /*fw_version=*/1,
                                         /*metric_count=*/1, /*offered_rate_hz=*/2});
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();
    ASSERT_TRUE(conn.try_write());
    ReadOneFrameFromServer();  // drain the CONFIG reply

    SendFromDevice(proto::MessageType::kSample,
                    proto::SamplePayload{/*session_id=*/1, /*timestamp_ms=*/4242,
                                          /*value_milli=*/1000, /*metric_id=*/0});
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();

    ASSERT_TRUE(ing.has_session());
    EXPECT_EQ(ing.session()->last_uptime_ms(), 4242u);
}

TEST_F(IngestTest, HeartbeatUpdatesTheSessionsLastUptimeMs) {
    server::event_loop loop;
    server::connection conn(loop, fds_[0]);
    server::store data_store;
    server::ingest ing(conn, /*session_id=*/1, data_store);

    SendFromDevice(proto::MessageType::kHello,
                    proto::HelloPayload{/*device_id=*/1, /*fw_version=*/1,
                                         /*metric_count=*/1, /*offered_rate_hz=*/2});
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();
    ASSERT_TRUE(conn.try_write());
    ReadOneFrameFromServer();  // drain the CONFIG reply

    SendFromDevice(proto::MessageType::kHeartbeat,
                    proto::HeartbeatPayload{/*session_id=*/1, /*uptime_ms=*/9000});
    ASSERT_TRUE(conn.try_read());
    ing.on_readable();

    ASSERT_TRUE(ing.has_session());
    EXPECT_EQ(ing.session()->last_uptime_ms(), 9000u);
}
