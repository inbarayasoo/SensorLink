#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <utility>

#include "proto/static_vector.hpp"

using proto::static_vector;

TEST(StaticVector, StartsEmpty) {
    static_vector<int, 4> v;
    EXPECT_TRUE(v.empty());
    EXPECT_FALSE(v.full());
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 4u);
}

TEST(StaticVector, PushBackFillsUpThenRefuses) {
    static_vector<int, 3> v;
    EXPECT_TRUE(v.push_back(10));
    EXPECT_TRUE(v.push_back(20));
    EXPECT_TRUE(v.push_back(30));
    EXPECT_TRUE(v.full());
    EXPECT_FALSE(v.push_back(40));  // refused, not undefined behaviour
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 10);
    EXPECT_EQ(v[2], 30);
}

TEST(StaticVector, MovePushBackAlsoRefusesWhenFull) {
    static_vector<int, 2> v;
    int a = 1;
    int b = 2;
    int c = 3;
    EXPECT_TRUE(v.push_back(std::move(a)));
    EXPECT_TRUE(v.push_back(std::move(b)));
    EXPECT_FALSE(v.push_back(std::move(c)));  // rvalue overload, full
    EXPECT_EQ(v.size(), 2u);
}

TEST(StaticVector, PopBackAndClear) {
    static_vector<int, 4> v{1, 2, 3};
    v.pop_back();
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v.back(), 2);
    v.clear();
    EXPECT_TRUE(v.empty());
}

TEST(StaticVector, InitializerListConstruction) {
    static_vector<int, 8> v{5, 6, 7};
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v.front(), 5);
    EXPECT_EQ(v.back(), 7);
}

TEST(StaticVector, IterationAndStorageAreContiguous) {
    static_vector<int, 8> v{1, 2, 3, 4};
    int sum = 0;
    for (const int x : v) {
        sum += x;
    }
    EXPECT_EQ(sum, 10);
    EXPECT_EQ(v.data() + v.size(), v.end());
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 10);
}

TEST(StaticVector, WorksAsAByteBuffer) {
    static_vector<std::uint8_t, 4> buf;
    EXPECT_TRUE(buf.push_back(0xDE));
    EXPECT_TRUE(buf.push_back(0xAD));
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf[0], 0xDE);
    EXPECT_EQ(buf[1], 0xAD);
}

TEST(StaticVector, UsableInConstantExpression) {
    constexpr int sum = [] {
        static_vector<int, 5> v;
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        int s = 0;
        for (const int x : v) {
            s += x;
        }
        return s;
    }();
    static_assert(sum == 6, "static_vector must be usable at compile time");
    SUCCEED();
}
