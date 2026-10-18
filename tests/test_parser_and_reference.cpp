#include <gtest/gtest.h>
#include "orderbook/itch_parser.hpp"
#include "orderbook/reference_book.hpp"

using namespace l3;

TEST(ItchParserTest, EndianHelpers) {
    uint8_t u16_data[] = {0x12, 0x34};
    EXPECT_EQ(read_u16_be(u16_data), 0x1234);

    uint8_t u32_data[] = {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(read_u32_be(u32_data), 0x12345678);

    uint8_t u48_data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_EQ(read_u48_be(u48_data), 0x000102030405ULL);

    uint8_t u64_data[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    EXPECT_EQ(read_u64_be(u64_data), 0x123456789ABCDEF0ULL);
}

TEST(ReferenceOrderBookTest, BasicLifecycle) {
    ReferenceOrderBook book("TEST", 1);

    // 1. Add Bid: 100 shares @ $150.00 (1,500,000)
    book.add_order(1001, Side::Buy, 100, 1500000);
    EXPECT_EQ(book.resting_orders_count(), 1);
    EXPECT_EQ(book.bid_levels_count(), 1);
    auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->first, 1500000);
    EXPECT_EQ(bb->second, 100);

    // 2. Add another Bid at higher price: 200 shares @ $150.50 (1,505,000)
    book.add_order(1002, Side::Buy, 200, 1505000);
    EXPECT_EQ(book.resting_orders_count(), 2);
    EXPECT_EQ(book.bid_levels_count(), 2);
    bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->first, 1505000);
    EXPECT_EQ(bb->second, 200);

    // 3. Add Ask: 300 shares @ $151.00 (1,510,000)
    book.add_order(2001, Side::Sell, 300, 1510000);
    auto ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->first, 1510000);
    EXPECT_EQ(ba->second, 300);

    // 4. Partial execute top bid
    book.execute_order(1002, 50);
    bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->first, 1505000);
    EXPECT_EQ(bb->second, 150);

    // 5. Complete execute top bid
    book.execute_order(1002, 150);
    EXPECT_EQ(book.resting_orders_count(), 2);
    EXPECT_EQ(book.bid_levels_count(), 1);
    bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->first, 1500000);
    EXPECT_EQ(bb->second, 100);

    // 6. Replace ask order: from 300 @ 151.00 to 400 @ 150.80
    book.replace_order(2001, 2002, 400, 1508000);
    EXPECT_EQ(book.resting_orders_count(), 2);
    ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->first, 1508000);
    EXPECT_EQ(ba->second, 400);

    // 7. Delete remaining orders
    book.delete_order(1001);
    book.delete_order(2002);
    EXPECT_EQ(book.resting_orders_count(), 0);
    EXPECT_EQ(book.bid_levels_count(), 0);
    EXPECT_EQ(book.ask_levels_count(), 0);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(ReferenceOrderBookTest, ReplayTestSlice) {
    struct TestHandler {
        ReferenceOrderBook book{"TEST", 7451}; // SPY locate
        size_t adds{0}, deletes{0}, executes{0};

        void on_system_event(const SystemEventMsg&) {}
        void on_stock_directory(const StockDirectoryMsg&) {}
        void on_add_order(const AddOrderMsg& m) {
            if (m.header.stock_locate == 7451) {
                book.add_order(m.order_ref, m.side, m.shares, m.price);
                adds++;
            }
        }
        void on_order_executed(const OrderExecutedMsg& m) {
            if (m.header.stock_locate == 7451) {
                book.execute_order(m.order_ref, m.executed_shares);
                executes++;
            }
        }
        void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
            if (m.header.stock_locate == 7451) {
                book.execute_order(m.order_ref, m.executed_shares);
                executes++;
            }
        }
        void on_order_cancel(const OrderCancelMsg& m) {
            if (m.header.stock_locate == 7451) {
                book.cancel_order(m.order_ref, m.canceled_shares);
            }
        }
        void on_order_delete(const OrderDeleteMsg& m) {
            if (m.header.stock_locate == 7451) {
                book.delete_order(m.order_ref);
                deletes++;
            }
        }
        void on_order_replace(const OrderReplaceMsg& m) {
            if (m.header.stock_locate == 7451) {
                book.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price);
            }
        }
        void on_trade(const TradeMsg&) {}
    };

    TestHandler handler;
    size_t count = ItchParser::parse_file("data/fixture_3symbols.bin.gz", handler);
    EXPECT_EQ(count, 16869);
    EXPECT_GT(handler.adds, 0);
    EXPECT_GT(handler.deletes, 0);

    auto stats = handler.book.get_stats();
    EXPECT_GT(stats.peak_resting_orders, 0);
    EXPECT_GT(stats.peak_bid_levels, 0);
    EXPECT_GT(stats.peak_ask_levels, 0);
}
