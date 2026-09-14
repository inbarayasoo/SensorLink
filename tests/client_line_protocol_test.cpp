#include "client/line_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::vector<std::uint8_t> ToBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace

TEST(ClientLineProtocolTest, ValidSampleLineIncrementsValidCount) {
    std::vector<std::uint8_t> buffer = ToBytes("temp 2000 5230\n");
    std::size_t valid = 0;
    std::size_t malformed = 0;

    client::handle_incoming(buffer, valid, malformed);

    EXPECT_EQ(valid, 1u);
    EXPECT_EQ(malformed, 0u);
    EXPECT_TRUE(buffer.empty());
}

TEST(ClientLineProtocolTest, MalformedLineIncrementsMalformedCount) {
    std::vector<std::uint8_t> buffer = ToBytes("not a valid sample line\n");
    std::size_t valid = 0;
    std::size_t malformed = 0;

    client::handle_incoming(buffer, valid, malformed);

    EXPECT_EQ(valid, 0u);
    EXPECT_EQ(malformed, 1u);
}

TEST(ClientLineProtocolTest, PartialLineIsLeftInTheBufferUntilItCompletes) {
    std::vector<std::uint8_t> buffer = ToBytes("temp 200");
    std::size_t valid = 0;
    std::size_t malformed = 0;

    client::handle_incoming(buffer, valid, malformed);
    EXPECT_EQ(valid, 0u);
    EXPECT_EQ(malformed, 0u);
    EXPECT_FALSE(buffer.empty());  // no newline yet -- nothing to act on

    const std::vector<std::uint8_t> rest = ToBytes("0 5230\n");
    buffer.insert(buffer.end(), rest.begin(), rest.end());
    client::handle_incoming(buffer, valid, malformed);

    EXPECT_EQ(valid, 1u);
    EXPECT_TRUE(buffer.empty());
}

TEST(ClientLineProtocolTest, CrlfLineEndingIsTolerated) {
    std::vector<std::uint8_t> buffer = ToBytes("temp 2000 5230\r\n");
    std::size_t valid = 0;
    std::size_t malformed = 0;

    client::handle_incoming(buffer, valid, malformed);

    EXPECT_EQ(valid, 1u);
}

TEST(ClientLineProtocolTest, TwoLinesInOneReadAreBothProcessed) {
    std::vector<std::uint8_t> buffer = ToBytes("temp 1000 5000\npressure 2000 990\n");
    std::size_t valid = 0;
    std::size_t malformed = 0;

    client::handle_incoming(buffer, valid, malformed);

    EXPECT_EQ(valid, 2u);
    EXPECT_EQ(malformed, 0u);
    EXPECT_TRUE(buffer.empty());
}
