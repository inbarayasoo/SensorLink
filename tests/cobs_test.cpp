#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "proto/cobs.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes encode(const Bytes& raw) {
    Bytes out(proto::cobs_encoded_max_size(raw.size()));
    const auto r = proto::cobs_encode(raw.data(), raw.size(), out.data(), out.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kOk);
    out.resize(r.status == proto::CobsStatus::kOk ? r.size : 0);
    return out;
}

std::optional<Bytes> decode(const Bytes& enc) {
    Bytes out(enc.size() + 1);  // decoded output is always shorter than the input
    const auto r = proto::cobs_decode(enc.data(), enc.size(), out.data(), out.size());
    if (r.status != proto::CobsStatus::kOk) {
        return std::nullopt;
    }
    out.resize(r.size);
    return out;
}

void expect_roundtrip(const Bytes& raw) {
    const Bytes enc = encode(raw);
    for (const std::uint8_t b : enc) {
        EXPECT_NE(b, 0) << "encoded output must never contain a zero byte";
    }
    EXPECT_LE(enc.size(), proto::cobs_encoded_max_size(raw.size()));

    const auto dec = decode(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, raw);
}

Bytes iota_bytes(int first, int count) {
    Bytes v;
    for (int i = 0; i < count; ++i) {
        v.push_back(static_cast<std::uint8_t>((first + i) & 0xFF));
    }
    return v;
}

}  // namespace

TEST(Cobs, KnownEncodings) {
    EXPECT_EQ(encode({0x00}), (Bytes{0x01, 0x01}));
    EXPECT_EQ(encode({0x00, 0x00}), (Bytes{0x01, 0x01, 0x01}));
    EXPECT_EQ(encode({0x11, 0x22, 0x00, 0x33}), (Bytes{0x03, 0x11, 0x22, 0x02, 0x33}));
    EXPECT_EQ(encode({0x11, 0x22, 0x33, 0x44}), (Bytes{0x05, 0x11, 0x22, 0x33, 0x44}));
    EXPECT_EQ(encode({0x11, 0x00, 0x00, 0x00}), (Bytes{0x02, 0x11, 0x01, 0x01, 0x01}));
}

TEST(Cobs, DecodesKnownVectorIndependently) {
    const auto d = decode({0x03, 0x11, 0x22, 0x02, 0x33});
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d, (Bytes{0x11, 0x22, 0x00, 0x33}));
}

TEST(Cobs, RoundTripSmallInputs) {
    expect_roundtrip({});
    expect_roundtrip({0x00});
    expect_roundtrip({0x00, 0x00, 0x00});
    expect_roundtrip({0x01});
    expect_roundtrip({0xFF});
    expect_roundtrip({0x11, 0x22, 0x33, 0x44});
    expect_roundtrip({0x11, 0x00, 0x22, 0x00, 0x33});
    expect_roundtrip({0x00, 0x11, 0x00});
}

TEST(Cobs, RoundTripBlockBoundaries) {
    expect_roundtrip(iota_bytes(1, 253));   // one byte short of a full block
    expect_roundtrip(iota_bytes(1, 254));   // exactly a full block
    expect_roundtrip(iota_bytes(1, 255));   // one past a full block
    expect_roundtrip(iota_bytes(1, 508));   // exactly two full blocks

    Bytes full_then_zero = iota_bytes(1, 254);
    full_then_zero.push_back(0x00);
    expect_roundtrip(full_then_zero);
}

TEST(Cobs, RoundTripLongMixedPattern) {
    expect_roundtrip(iota_bytes(0, 300));   // wraps through 0x00 twice
    expect_roundtrip(Bytes(1000, 0x00));    // 1000 zeros
    expect_roundtrip(Bytes(1000, 0xAB));    // 1000 non-zeros
}

TEST(Cobs, EncodeReportsOutputOverflow) {
    const Bytes raw{0x11, 0x22, 0x33};      // needs 4 bytes
    std::array<std::uint8_t, 2> tiny{};
    const auto r = proto::cobs_encode(raw.data(), raw.size(), tiny.data(), tiny.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kOutputOverflow);
}

TEST(Cobs, EncodeReportsOutputOverflowOnZeroCapacity) {
    const Bytes raw{0x11};
    std::array<std::uint8_t, 1> out{};
    const auto r = proto::cobs_encode(raw.data(), raw.size(), out.data(), 0);
    EXPECT_EQ(r.status, proto::CobsStatus::kOutputOverflow);
}

TEST(Cobs, DecodeReportsOutputOverflow) {
    const Bytes enc = encode({0x11, 0x22, 0x33, 0x44});  // decodes to 4 bytes
    std::array<std::uint8_t, 3> tiny{};
    const auto r = proto::cobs_decode(enc.data(), enc.size(), tiny.data(), tiny.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kOutputOverflow);
}

TEST(Cobs, DecodeRejectsZeroByte) {
    const Bytes bad{0x03, 0x11, 0x00, 0x33};  // a 0x00 cannot appear in COBS data
    std::array<std::uint8_t, 16> out{};
    const auto r = proto::cobs_decode(bad.data(), bad.size(), out.data(), out.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kInvalidInput);
}

TEST(Cobs, DecodeRejectsTruncatedBlock) {
    const Bytes trunc{0x05, 0x11, 0x22};  // code promises 4 data bytes, only 2 present
    std::array<std::uint8_t, 16> out{};
    const auto r = proto::cobs_decode(trunc.data(), trunc.size(), out.data(), out.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kInvalidInput);
}

TEST(Cobs, DecodeRejectsZeroCodeByteMidStream) {
    const Bytes bad{0x02, 0x11, 0x00, 0x11};  // second code byte is 0x00
    std::array<std::uint8_t, 16> out{};
    const auto r = proto::cobs_decode(bad.data(), bad.size(), out.data(), out.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kInvalidInput);
}

TEST(Cobs, DecodeOverflowOnTheImpliedZero) {
    const Bytes enc = encode({0x11, 0x00});  // -> {0x02, 0x11, 0x01, 0x01}
    std::array<std::uint8_t, 1> tiny{};      // room for 0x11 but not its trailing zero
    const auto r = proto::cobs_decode(enc.data(), enc.size(), tiny.data(), tiny.size());
    EXPECT_EQ(r.status, proto::CobsStatus::kOutputOverflow);
}

namespace {

constexpr bool cobs_roundtrips_in_constant_expression() {
    const std::array<std::uint8_t, 5> raw{0x11, 0x00, 0x22, 0x00, 0x33};
    std::array<std::uint8_t, 16> enc{};
    const auto e = proto::cobs_encode(raw.data(), raw.size(), enc.data(), enc.size());
    if (e.status != proto::CobsStatus::kOk) {
        return false;
    }
    std::array<std::uint8_t, 16> dec{};
    const auto d = proto::cobs_decode(enc.data(), e.size, dec.data(), dec.size());
    if (d.status != proto::CobsStatus::kOk || d.size != raw.size()) {
        return false;
    }
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (dec[i] != raw[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(Cobs, WorksInConstantExpressions) {
    static_assert(cobs_roundtrips_in_constant_expression(),
                  "COBS encode/decode must round-trip at compile time");
    SUCCEED();
}
