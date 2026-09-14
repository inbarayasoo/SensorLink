#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "proto/ring_buffer.hpp"

using proto::ring_buffer;

TEST(RingBuffer, SingleThreadedFillAndDrain) {
    ring_buffer<int, 4> rb;  // holds 3 elements
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.capacity(), 3u);

    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    EXPECT_TRUE(rb.push(3));
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.push(4));  // full: value is dropped, not overwritten

    int v = 0;
    EXPECT_TRUE(rb.pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(rb.pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_TRUE(rb.pop(v));
    EXPECT_EQ(v, 3);
    EXPECT_FALSE(rb.pop(v));
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, IndicesWrapAround) {
    ring_buffer<int, 4> rb;
    int v = 0;
    for (int round = 0; round < 100; ++round) {
        EXPECT_TRUE(rb.push(round));
        EXPECT_TRUE(rb.pop(v));
        EXPECT_EQ(v, round);
    }
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, SizeTracksContents) {
    ring_buffer<int, 8> rb;
    EXPECT_EQ(rb.size(), 0u);
    rb.push(1);
    rb.push(2);
    rb.push(3);
    EXPECT_EQ(rb.size(), 3u);
    int v = 0;
    rb.pop(v);
    EXPECT_EQ(rb.size(), 2u);

    while (rb.pop(v)) {
    }
    EXPECT_FALSE(rb.pop(v));  // empty: pop refuses
    while (rb.push(0)) {
    }
    EXPECT_FALSE(rb.push(0));  // full: push refuses
    EXPECT_TRUE(rb.full());
}

TEST(RingBuffer, ProducerConsumerStreamArrivesIntactAndInOrder) {
    ring_buffer<std::uint32_t, 64> rb;
    constexpr std::uint32_t kCount = 200000;

    std::vector<std::uint32_t> received;
    received.reserve(kCount);

    std::thread consumer([&] {
        std::uint32_t value = 0;
        std::uint32_t count = 0;
        while (count < kCount) {
            if (rb.pop(value)) {
                received.push_back(value);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (std::uint32_t i = 0; i < kCount; ++i) {
        while (!rb.push(i)) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    ASSERT_EQ(received.size(), kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[i], i);
    }
}
