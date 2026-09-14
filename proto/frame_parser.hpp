#pragma once

#include <cstddef>
#include <cstdint>

#include "proto/cobs.hpp"
#include "proto/crc16.hpp"
#include "proto/messages.hpp"

// Turns a raw byte stream back into whole, validated frames. This is the mirror
// image of frame_encoder, and the same code runs on the server.
//
// Wire framing (see messages.hpp):
//   COBS( type | length | payload | crc16 big-endian )  0x00
//
// The parser is a small table-driven machine that consumes one byte at a time:
//
//   kHunting          - between frames. 0x00 is ignored. The first non-zero
//                       byte starts a frame.
//   kInFrame          - buffering COBS bytes. 0x00 ends the frame: COBS-decode,
//                       check the length field and the CRC, then deliver or
//                       drop.
//   kSkipToDelimiter  - lost sync mid-frame (the buffer filled with no 0x00 in
//                       sight). Ignore everything until the next 0x00, which is
//                       a known frame boundary, then resume hunting.
//
// A malformed frame is dropped and the parser resynchronises on the next 0x00.
// One lost or corrupted byte costs at most one frame.
namespace proto {

enum class ParseError : std::uint8_t {
    kNone,
    kCobsError,     // COBS layer rejected the buffered bytes
    kBadLength,     // decoded size disagrees with the length field
    kBadCrc,        // CRC check failed
    kFrameTooLong,  // no 0x00 arrived before the buffer filled
};

// A decoded, CRC-checked frame. payload points into the parser's own storage
// and stays valid only until the next push_byte / push_bytes call.
struct ParsedFrame {
    MessageType type;
    const std::uint8_t* payload;
    std::size_t payload_size;
};

// MaxPayload caps the payload the parser will accept; the internal buffers are
// sized from it, so a parser instance costs a known, fixed amount of RAM.
template <std::size_t MaxPayload = kMaxPayloadSize>
class frame_parser {
public:
    static constexpr std::size_t max_payload_size() { return MaxPayload; }

    // Feed one byte. Returns a pointer to a freshly completed frame, or nullptr
    // if this byte did not complete one (including when a frame was dropped;
    // check last_error() in that case).
    const ParsedFrame* push_byte(std::uint8_t byte) {
        last_error_ = ParseError::kNone;

        const std::size_t row = static_cast<std::size_t>(state_);
        const std::size_t col = (byte == 0x00) ? 1u : 0u;
        const Transition step = kTable[row][col];

        const ParsedFrame* completed = nullptr;
        switch (step.action) {
            case Action::kIgnore:
                break;
            case Action::kStart:
                buffer_[0] = byte;
                buffered_ = 1;
                break;
            case Action::kAppend:
                if (buffered_ == kBufferSize) {
                    fail(ParseError::kFrameTooLong);
                    buffered_ = 0;
                    state_ = State::kSkipToDelimiter;
                    return nullptr;
                }
                buffer_[buffered_++] = byte;
                break;
            case Action::kComplete:
                completed = finish_frame();
                buffered_ = 0;
                break;
        }
        state_ = step.next;
        return completed;
    }

    // Feed a run of bytes, handing each completed frame to sink, a callable
    // taking (const ParsedFrame&).
    template <typename Sink>
    void push_bytes(const std::uint8_t* data, std::size_t size, Sink&& sink) {
        for (std::size_t i = 0; i < size; ++i) {
            if (const ParsedFrame* frame = push_byte(data[i])) {
                sink(*frame);
            }
        }
    }

    ParseError last_error() const { return last_error_; }
    std::size_t frames_ok() const { return frames_ok_; }
    std::size_t frames_dropped() const { return frames_dropped_; }

private:
    enum class State : std::uint8_t {
        kHunting = 0,
        kInFrame = 1,
        kSkipToDelimiter = 2,
    };
    enum class Action : std::uint8_t { kIgnore, kStart, kAppend, kComplete };

    struct Transition {
        State next;
        Action action;
    };

    // kTable[state][is_delimiter]
    static constexpr Transition kTable[3][2] = {
        // byte != 0x00                              byte == 0x00
        {{State::kInFrame, Action::kStart}, {State::kHunting, Action::kIgnore}},
        {{State::kInFrame, Action::kAppend}, {State::kHunting, Action::kComplete}},
        {{State::kSkipToDelimiter, Action::kIgnore}, {State::kHunting, Action::kIgnore}},
    };

    static constexpr std::size_t kMaxBlob =
        kFrameHeaderSize + MaxPayload + kFrameCrcSize;
    static constexpr std::size_t kBufferSize = cobs_encoded_max_size(kMaxBlob);

    const ParsedFrame* finish_frame() {
        const CobsResult decoded =
            cobs_decode(buffer_, buffered_, blob_, sizeof(blob_));
        if (decoded.status != CobsStatus::kOk) {
            fail(ParseError::kCobsError);
            return nullptr;
        }
        if (decoded.size < kFrameHeaderSize + kFrameCrcSize) {
            fail(ParseError::kBadLength);
            return nullptr;
        }

        const std::size_t declared = blob_[1];
        if (decoded.size != kFrameHeaderSize + declared + kFrameCrcSize) {
            fail(ParseError::kBadLength);
            return nullptr;
        }

        // Residue over the whole decoded blob (payload data then its CRC) is 0.
        if (crc16(blob_, decoded.size) != 0) {
            fail(ParseError::kBadCrc);
            return nullptr;
        }

        frame_.type = static_cast<MessageType>(blob_[0]);
        frame_.payload = blob_ + kFrameHeaderSize;
        frame_.payload_size = declared;
        ++frames_ok_;
        return &frame_;
    }

    void fail(ParseError error) {
        last_error_ = error;
        ++frames_dropped_;
    }

    State state_ = State::kHunting;
    std::size_t buffered_ = 0;
    std::uint8_t buffer_[kBufferSize] = {};
    std::uint8_t blob_[kMaxBlob] = {};
    ParsedFrame frame_{};

    ParseError last_error_ = ParseError::kNone;
    std::size_t frames_ok_ = 0;
    std::size_t frames_dropped_ = 0;
};

}  // namespace proto