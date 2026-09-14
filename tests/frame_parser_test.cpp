#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "proto/cobs.hpp"
#include "proto/crc16.hpp"
#include "proto/frame_encoder.hpp"
#include "proto/frame_parser.hpp"

using namespace proto;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes make_frame(MessageType type, const Bytes& payload) {
    Bytes out(max_encoded_frame_size(payload.size()));
    const auto r = encode_frame(type, payload.data(), payload.size(),
                                out.data(), out.size());
    EXPECT_EQ(r.status, EncodeStatus::kOk);
    out.resize(r.status == EncodeStatus::kOk ? r.size : 0);
    return out;
}

// COBS-frame an arbitrary blob, bypassing encode_frame so we can build broken
// frames on purpose.
Bytes frame_raw_blob(const Bytes& blob) {
    Bytes out(cobs_encoded_max_size(blob.size()) + 1);
    const auto r = cobs_encode(blob.data(), blob.size(), out.data(), out.size() - 1);
    EXPECT_EQ(r.status, CobsStatus::kOk);
    out.resize(r.size + 1);
    out.back() = 0x00;
    return out;
}

// A structurally valid blob (type | length | payload | crc) whose stored CRC is
// deliberately wrong by one bit.
Bytes blob_with_wrong_crc(MessageType type, const Bytes& payload) {
    Bytes blob;
    blob.push_back(static_cast<std::uint8_t>(type));
    blob.push_back(static_cast<std::uint8_t>(payload.size()));
    blob.insert(blob.end(), payload.begin(), payload.end());
    const std::uint16_t wrong =
        static_cast<std::uint16_t>(crc16(blob.data(), blob.size()) ^ 0x0100u);
    blob.push_back(static_cast<std::uint8_t>(wrong >> 8));
    blob.push_back(static_cast<std::uint8_t>(wrong & 0xFFu));
    return blob;
}

// Feed every byte, collecting completed frames as (type, payload) pairs.
std::vector<std::pair<MessageType, Bytes>> run(frame_parser<>& parser,
                                               const Bytes& stream) {
    std::vector<std::pair<MessageType, Bytes>> frames;
    for (const std::uint8_t b : stream) {
        if (const ParsedFrame* f = parser.push_byte(b)) {
            frames.emplace_back(f->type, Bytes(f->payload, f->payload + f->payload_size));
        }
    }
    return frames;
}

}  // namespace

TEST(FrameParser, DecodesASingleFrame) {
    frame_parser<> parser;
    const Bytes payload{0x10, 0x20, 0x00, 0x30, 0x40};
    const auto frames = run(parser, make_frame(MessageType::kSample, payload));

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].first, MessageType::kSample);
    EXPECT_EQ(frames[0].second, payload);
    EXPECT_EQ(parser.frames_ok(), 1u);
    EXPECT_EQ(parser.frames_dropped(), 0u);
    EXPECT_EQ(parser.last_error(), ParseError::kNone);
}

TEST(FrameParser, DecodesBackToBackFrames) {
    frame_parser<> parser;
    Bytes stream = make_frame(MessageType::kHello, {0x01, 0x02});
    const Bytes second = make_frame(MessageType::kHeartbeat, {});
    stream.insert(stream.end(), second.begin(), second.end());

    const auto frames = run(parser, stream);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].first, MessageType::kHello);
    EXPECT_EQ(frames[1].first, MessageType::kHeartbeat);
    EXPECT_EQ(parser.frames_ok(), 2u);
}

TEST(FrameParser, IgnoresLoneDelimiters) {
    frame_parser<> parser;
    const auto frames = run(parser, Bytes{0x00, 0x00, 0x00});
    EXPECT_TRUE(frames.empty());
    EXPECT_EQ(parser.frames_ok(), 0u);
    EXPECT_EQ(parser.frames_dropped(), 0u);
}

TEST(FrameParser, ResyncsAfterLeadingGarbage) {
    frame_parser<> parser;
    Bytes stream{0x7A, 0x11, 0x42, 0x00};  // junk, then a delimiter
    const Bytes good = make_frame(MessageType::kConfig, {0xAA, 0xBB, 0xCC, 0xDD});
    stream.insert(stream.end(), good.begin(), good.end());

    const auto frames = run(parser, stream);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].first, MessageType::kConfig);
    EXPECT_GE(parser.frames_dropped(), 1u);  // the junk was rejected
    EXPECT_EQ(parser.frames_ok(), 1u);
}

TEST(FrameParser, DropsBadFrameThenResyncs) {
    frame_parser<> parser;
    Bytes stream = frame_raw_blob(blob_with_wrong_crc(MessageType::kSample,
                                                      {0x11, 0x22, 0x33, 0x44}));
    const Bytes good = make_frame(MessageType::kSample, {0x09, 0x08, 0x07});
    stream.insert(stream.end(), good.begin(), good.end());

    const auto frames = run(parser, stream);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].second, (Bytes{0x09, 0x08, 0x07}));
    EXPECT_EQ(parser.frames_dropped(), 1u);
    EXPECT_EQ(parser.frames_ok(), 1u);
}

TEST(FrameParser, ReportsBadCrc) {
    frame_parser<> parser;
    run(parser, frame_raw_blob(blob_with_wrong_crc(MessageType::kSample,
                                                   {0x42, 0x42, 0x42, 0x42})));
    EXPECT_EQ(parser.last_error(), ParseError::kBadCrc);
    EXPECT_EQ(parser.frames_ok(), 0u);
}

TEST(FrameParser, ReportsBadLength) {
    frame_parser<> parser;
    // type | length says 9 | 3 payload bytes | 2 crc bytes  -> sizes disagree
    const Bytes blob{static_cast<std::uint8_t>(MessageType::kSample), 0x09,
                     0x01, 0x02, 0x03, 0x00, 0x00};
    run(parser, frame_raw_blob(blob));
    EXPECT_EQ(parser.last_error(), ParseError::kBadLength);
}

TEST(FrameParser, ReportsCobsError) {
    frame_parser<> parser;
    // 0x05 promises four data bytes but the frame ends after two.
    run(parser, Bytes{0x05, 0x01, 0x02, 0x00});
    EXPECT_EQ(parser.last_error(), ParseError::kCobsError);
}

TEST(FrameParser, ReportsTooShortFrame) {
    frame_parser<> parser;
    run(parser, frame_raw_blob(Bytes{0x01, 0x02}));  // only 2 bytes, need >= 4
    EXPECT_EQ(parser.last_error(), ParseError::kBadLength);
}

TEST(FrameParser, ReportsFrameTooLongThenResyncs) {
    frame_parser<> parser;
    Bytes stream(4096, 0x7F);  // no delimiter, far past the internal buffer
    stream.push_back(0x00);    // the boundary the parser resyncs on
    const Bytes good = make_frame(MessageType::kHeartbeat, {0x01});
    stream.insert(stream.end(), good.begin(), good.end());

    const auto frames = run(parser, stream);
    EXPECT_EQ(parser.last_error(), ParseError::kNone);  // cleared by the good frame
    EXPECT_GE(parser.frames_dropped(), 1u);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].first, MessageType::kHeartbeat);
}

TEST(FrameParser, PushBytesDeliversEveryFrameToSink) {
    frame_parser<> parser;
    Bytes stream;
    for (int i = 0; i < 3; ++i) {
        const Bytes f =
            make_frame(MessageType::kSample, Bytes{static_cast<std::uint8_t>(i)});
        stream.insert(stream.end(), f.begin(), f.end());
    }

    std::vector<MessageType> seen;
    parser.push_bytes(stream.data(), stream.size(),
                      [&](const ParsedFrame& f) { seen.push_back(f.type); });

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(parser.frames_ok(), 3u);
}

TEST(FrameParser, MaxSizePayloadRoundTrips) {
    frame_parser<> parser;
    Bytes payload(kMaxPayloadSize, 0x00);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFFu);
    }
    const auto frames = run(parser, make_frame(MessageType::kSample, payload));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].second, payload);
}
