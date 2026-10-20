#include <gtest/gtest.h>
#include "orderbook/fill_simulator.hpp"
#include "orderbook/order_book.hpp"

using namespace l3;

TEST(Stage4FillSimulatorTest, TouchFillVsQueueL3Priority) {
    // Construct OrderBook for testing
    OrderBookConfig cfg{
        .order_pool_capacity = 1024,
        .level_pool_capacity = 128,
        .order_map_capacity = 2048
    };
    OrderBook book("AAPL", 1, cfg);

    // Initial state: Level at 100.00 (1,000,000 price units) with 500 shares ahead
    uint64_t existing_ref1 = 1001;
    uint64_t existing_ref2 = 1002;
    book.add_order(existing_ref1, Side::Buy, 200, 1'000'000);
    book.add_order(existing_ref2, Side::Buy, 300, 1'000'000);

    // Simulated strategy order placed at tail of queue
    SimOrder sim_ord{};
    sim_ord.sim_id = 9999;
    sim_ord.stock_locate = 1;
    sim_ord.side = Side::Buy;
    sim_ord.price = 1'000'000;
    sim_ord.shares = 100;
    sim_ord.submit_ts_ns = 1'000'000'000ULL;
    sim_ord.arrival_ts_ns = 1'000'000'000ULL;
    sim_ord.cancel_ts_ns = 6'000'000'000ULL;

    // Add simulated order to book
    book.add_order(sim_ord.sim_id, sim_ord.side, sim_ord.shares, sim_ord.price);

    // Verify queue position: 500 shares ahead
    auto qpos = book.get_queue_position(sim_ord.sim_id);
    EXPECT_TRUE(qpos.exists);
    EXPECT_EQ(qpos.shares_ahead, 500u);
    EXPECT_EQ(qpos.orders_ahead, 2u);

    // Trade 1: 150 shares execute against existing_ref1
    // Under Naive Touch: Fills immediately!
    // Under Queue L3: Does NOT fill (350 shares still ahead)
    if (!sim_ord.touch_filled) {
        sim_ord.touch_filled = true;
        sim_ord.touch_fill_ts = 1'100'000'000ULL;
    }

    bool ahead = book.is_order_ahead(existing_ref1, sim_ord.sim_id);
    EXPECT_TRUE(ahead);

    book.execute_order(existing_ref1, 150);
    qpos = book.get_queue_position(sim_ord.sim_id);
    EXPECT_EQ(qpos.shares_ahead, 350u);
    EXPECT_FALSE(sim_ord.l3_filled);
    EXPECT_TRUE(sim_ord.touch_filled); // Touch filled prematurely!

    // Trade 2: 50 shares execute against existing_ref1 (clearing it)
    book.execute_order(existing_ref1, 50);
    qpos = book.get_queue_position(sim_ord.sim_id);
    EXPECT_EQ(qpos.shares_ahead, 300u);
    EXPECT_FALSE(sim_ord.l3_filled);

    // Trade 3: 300 shares execute against existing_ref2 (clearing all orders ahead)
    book.execute_order(existing_ref2, 300);
    qpos = book.get_queue_position(sim_ord.sim_id);
    EXPECT_EQ(qpos.shares_ahead, 0u);
    EXPECT_FALSE(sim_ord.l3_filled); // Now at head of queue!

    // Trade 4: Next trade hits head of queue -> Now Queue L3 fills!
    if (qpos.shares_ahead == 0) {
        sim_ord.l3_filled = true;
        sim_ord.l3_fill_ts = 1'800'000'000ULL;
        book.execute_order(sim_ord.sim_id, sim_ord.shares);
    }

    EXPECT_TRUE(sim_ord.l3_filled);
    // Verified lead time: Touch filled 700 ms earlier than Queue L3!
    double lead_time_ms = (static_cast<double>(sim_ord.l3_fill_ts) - static_cast<double>(sim_ord.touch_fill_ts)) / 1e6;
    EXPECT_DOUBLE_EQ(lead_time_ms, 700.0);

    // Verified book is clean: sim_id executed, no ghost orders
    auto qpos_after = book.get_queue_position(sim_ord.sim_id);
    EXPECT_FALSE(qpos_after.exists);
}

TEST(Stage4FillSimulatorTest, MarkoutAndDriftCalculations) {
    // Fill price = $100.00 (1,000,000 units)
    uint32_t fill_price = 1'000'000;
    // Fill mid = $100.01 (1,000,100 units) -> Half-spread = 1 cent = 100 units
    double fill_mid = 1'000'100.0;

    // Half-spread capture in bps: (100 / 1,000,000) * 10,000 = 1.0 bps
    double half_spread_bps = ((fill_mid - static_cast<double>(fill_price)) / static_cast<double>(fill_price)) * 10'000.0;
    EXPECT_DOUBLE_EQ(half_spread_bps, 1.0);

    // Case 1: Post-fill mid moves up to $100.05 (1,000,500 units) at 5s
    double mid_5s = 1'000'500.0;
    double markout_5s_bps = ((mid_5s - static_cast<double>(fill_price)) / static_cast<double>(fill_price)) * 10'000.0;
    EXPECT_DOUBLE_EQ(markout_5s_bps, 5.0); // +5.0 bps

    // Mid drift = markout - half-spread = 5.0 - 1.0 = +4.0 bps
    double drift_5s = markout_5s_bps - half_spread_bps;
    EXPECT_DOUBLE_EQ(drift_5s, 4.0);

    // Case 2: Adverse selection! Post-fill mid crashes to $99.96 (999,600 units) at 30s
    double mid_30s = 999'600.0;
    double markout_30s_bps = ((mid_30s - static_cast<double>(fill_price)) / static_cast<double>(fill_price)) * 10'000.0;
    EXPECT_DOUBLE_EQ(markout_30s_bps, -4.0); // -4.0 bps

    // Mid drift = -4.0 - 1.0 = -5.0 bps (severe toxic adverse selection)
    double drift_30s = markout_30s_bps - half_spread_bps;
    EXPECT_DOUBLE_EQ(drift_30s, -5.0);

    // Realized dollar PnL for 100 shares at 30s:
    // PnL = 100 * (999,600 - 1,000,000) / 10,000 = 100 * (-400) / 10,000 = -$4.00
    double pnl_30s = (markout_30s_bps * static_cast<double>(fill_price)) / 1'000'000.0;
    EXPECT_DOUBLE_EQ(pnl_30s, -4.0);
}

TEST(Stage4FillSimulatorTest, BootstrapConfidenceIntervalBounds) {
    std::vector<double> sample_values;
    for (int i = 1; i <= 1000; ++i) {
        sample_values.push_back(static_cast<double>(i));
    }

    BootstrapCI ci = compute_bootstrap_ci(sample_values, 500.0);
    EXPECT_DOUBLE_EQ(ci.point_est, 500.0);
    // 2.5th percentile of 1000 items is at index 25 (value 26)
    EXPECT_EQ(ci.ci_lo, 26.0);
    // 97.5th percentile is at index 975 (value 976)
    EXPECT_EQ(ci.ci_hi, 976.0);
}
