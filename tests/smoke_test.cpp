// Smoke test: proves the host toolchain, CMake wiring and GoogleTest all work
// before any real protocol code exists. Real component tests replace this over
// the next sub-steps.
#include <gtest/gtest.h>

#include "proto/version.hpp"

TEST(Smoke, ToolchainWorks) {
    EXPECT_EQ(2 + 2, 4);
}

TEST(Smoke, ProtoHeaderIsReachable) {
    EXPECT_STREQ(proto::kVersion, "0.1.0");
}
