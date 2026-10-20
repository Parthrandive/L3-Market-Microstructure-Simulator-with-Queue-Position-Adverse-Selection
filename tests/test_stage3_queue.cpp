#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <numeric>
#include "orderbook/ring_buffer.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/queue_tracker.hpp"

using namespace l3;

TEST(SpscRingBufferTest, BasicPushPopAndWrapAround) {
    constexpr size_t CAP = 8;
    SpscRingBuffer<uint32_t, CAP> rb;

    EXPECT_TRUE(rb.empty_approx());
    EXPECT_EQ(rb.size_approx(), 0);

    // Fill buffer (capacity is 8, but power-of-2 SPSC can hold up to CAP-1 or CAP elements)
    for (uint32_t i = 1; i <= 7; ++i) {
        EXPECT_TRUE(rb.try_push(i));
    }

    EXPECT_FALSE(rb.empty_approx());

    // Pop 3 items
    for (uint32_t i = 1; i <= 3; ++i) {
        uint32_t val = 0;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }

    // Push 3 more to test cyclic wrap-around
    for (uint32_t i = 8; i <= 10; ++i) {
        EXPECT_TRUE(rb.try_push(i));
    }

    // Pop remaining
    for (uint32_t i = 4; i <= 10; ++i) {
        uint32_t val = 0;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }

    EXPECT_TRUE(rb.empty_approx());
}

TEST(SpscRingBufferTest, MultiThreadedProducerConsumerTSan) {
    constexpr size_t CAP = 1024;
    constexpr size_t NUM_ITEMS = 500'000;
    SpscRingBuffer<uint64_t, CAP> rb;

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= NUM_ITEMS; ++i) {
            while (!rb.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        uint64_t expected = 1;
        while (expected <= NUM_ITEMS) {
            uint64_t val = 0;
            if (rb.try_pop(val)) {
                EXPECT_EQ(val, expected);
                expected++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(rb.empty_approx());
}

TEST(OrderBookQueueTest, GroundTruthQueuePositionTracking) {
    OrderBook book("AAPL");

    // Add 3 orders at $150.00 (Buy)
    // Order 1: 100 shares
    book.add_order(1001, Side::Buy, 100, 1500000);
    // Order 2: 200 shares
    book.add_order(1002, Side::Buy, 200, 1500000);
    // Order 3: 300 shares
    book.add_order(1003, Side::Buy, 300, 1500000);

    // Check Order 1 (head)
    auto pos1 = book.get_queue_position(1001);
    EXPECT_TRUE(pos1.exists);
    EXPECT_EQ(pos1.shares_ahead, 0);
    EXPECT_EQ(pos1.orders_ahead, 0);
    EXPECT_EQ(pos1.order_shares, 100);
    EXPECT_EQ(pos1.level_total_shares, 600);

    // Check Order 2 (middle)
    auto pos2 = book.get_queue_position(1002);
    EXPECT_TRUE(pos2.exists);
    EXPECT_EQ(pos2.shares_ahead, 100);
    EXPECT_EQ(pos2.orders_ahead, 1);
    EXPECT_EQ(pos2.order_shares, 200);

    // Check Order 3 (tail)
    auto pos3 = book.get_queue_position(1003);
    EXPECT_TRUE(pos3.exists);
    EXPECT_EQ(pos3.shares_ahead, 300); // 100 + 200
    EXPECT_EQ(pos3.orders_ahead, 2);
    EXPECT_EQ(pos3.order_shares, 300);

    // Partial cancel 50 shares of Order 1
    book.cancel_order(1001, 50);
    pos2 = book.get_queue_position(1002);
    EXPECT_EQ(pos2.shares_ahead, 50); // Order 1 now has 50 shares

    pos3 = book.get_queue_position(1003);
    EXPECT_EQ(pos3.shares_ahead, 250); // 50 + 200

    // Fully execute Order 1
    book.execute_order(1001, 50);
    pos1 = book.get_queue_position(1001);
    EXPECT_FALSE(pos1.exists);

    // Order 2 is now at head of queue
    pos2 = book.get_queue_position(1002);
    EXPECT_TRUE(pos2.exists);
    EXPECT_EQ(pos2.shares_ahead, 0);
    EXPECT_EQ(pos2.orders_ahead, 0);

    // Order 3 has only Order 2 ahead (200 shares)
    pos3 = book.get_queue_position(1003);
    EXPECT_EQ(pos3.shares_ahead, 200);
    EXPECT_EQ(pos3.orders_ahead, 1);
}

TEST(QueueTrackerTest, PessimisticVsProbabilisticModelUpdates) {
    L2QueueState pess{};
    L2QueueState prob{};

    // Initial state: 1000 shares ahead, our order is 100 shares
    pess.init(1000, 100);
    prob.init(1000, 100);

    // 1. Trade occurs of 400 shares
    pess.on_execution(400, 1000);
    prob.on_execution(400, 1000);

    EXPECT_DOUBLE_EQ(pess.queue_ahead, 600.0);
    EXPECT_DOUBLE_EQ(prob.queue_ahead, 600.0);
    EXPECT_FALSE(pess.filled);
    EXPECT_FALSE(prob.filled);

    // 2. Cancellation occurs:
    // Before cancel: level depth was 2000 shares (600 ahead, 100 ours, 1300 behind).
    // Cancel 500 shares. New depth = 1500 shares.
    pess.on_cancel_pessimistic(1500);
    // Probabilistic: fraction ahead = 600 / 2000 = 0.30
    // Expected cancels ahead = 500 * 0.30 = 150 shares
    // New queue ahead = 600 - 150 = 450 shares
    prob.on_cancel_probabilistic(500, 2000, 1500);

    // Pessimistic holds queue ahead at 600 (cancels assumed behind)
    EXPECT_DOUBLE_EQ(pess.queue_ahead, 600.0);
    // Probabilistic drops queue ahead to 450
    EXPECT_DOUBLE_EQ(prob.queue_ahead, 450.0);

    // 3. Trade occurs of 500 shares at timestamp 2000
    pess.on_execution(500, 2000);
    prob.on_execution(500, 2000);

    // Pessimistic had 600 ahead, now has 100 ahead (NOT filled)
    EXPECT_DOUBLE_EQ(pess.queue_ahead, 100.0);
    EXPECT_FALSE(pess.filled);

    // Probabilistic had 450 ahead. 500 shares executed:
    // 450 depleted ahead, remaining 50 executed on our order.
    EXPECT_DOUBLE_EQ(prob.queue_ahead, 0.0);
    EXPECT_EQ(prob.remaining_order_shares, 50); // 100 - 50
    EXPECT_FALSE(prob.filled);

    // 4. Another trade of 60 shares at timestamp 3000
    prob.on_execution(60, 3000);
    // 50 remaining filled!
    EXPECT_TRUE(prob.filled);
    EXPECT_EQ(prob.fill_timestamp_ns, 3000);
    EXPECT_EQ(prob.remaining_order_shares, 0);
}
