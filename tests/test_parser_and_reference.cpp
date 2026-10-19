#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <filesystem>
#include "orderbook/itch_parser.hpp"
#include "reference_book.hpp"

using namespace l3;

TEST(ItchParserTest, EndianHelpers) {
    uint8_t u16_data[] = {0x12, 0x34};
    EXPECT_EQ(read_u16_be(u16_data), 0x1234);

    uint8_t u32_data[] = {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(read_u32_be(u32_data), 0x12345678);

    // Timestamp with nonzero high bytes: 0x0102 in high 16 bits of 48-bit int
    uint8_t u48_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    EXPECT_EQ(read_u48_be(u48_data), 0x010203040506ULL);

    uint8_t u64_data[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    EXPECT_EQ(read_u64_be(u64_data), 0x123456789ABCDEF0ULL);
}

// Dummy handler that records parsed messages for precise field assertion
struct MockHandler {
    std::optional<SystemEventMsg> last_system_event;
    std::optional<StockDirectoryMsg> last_stock_directory;
    std::optional<AddOrderMsg> last_add_order;
    std::optional<OrderExecutedMsg> last_order_executed;
    std::optional<OrderExecutedWithPriceMsg> last_executed_with_price;
    std::optional<OrderCancelMsg> last_order_cancel;
    std::optional<OrderDeleteMsg> last_order_delete;
    std::optional<OrderReplaceMsg> last_order_replace;
    std::optional<TradeMsg> last_trade;

    void on_system_event(const SystemEventMsg& m) { last_system_event = m; }
    void on_stock_directory(const StockDirectoryMsg& m) { last_stock_directory = m; }
    void on_add_order(const AddOrderMsg& m) { last_add_order = m; }
    void on_order_executed(const OrderExecutedMsg& m) { last_order_executed = m; }
    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) { last_executed_with_price = m; }
    void on_order_cancel(const OrderCancelMsg& m) { last_order_cancel = m; }
    void on_order_delete(const OrderDeleteMsg& m) { last_order_delete = m; }
    void on_order_replace(const OrderReplaceMsg& m) { last_order_replace = m; }
    void on_trade(const TradeMsg& m) { last_trade = m; }
};

// Hand-build raw big-endian bytes for Add Order ('A')
TEST(ItchParserTest, HandBuiltAddOrder) {
    // Length: 36 bytes payload (indices 0..35)
    uint8_t raw[36] = {
        'A',                    // 0: type
        0x13, 0x37,             // 1..2: stock_locate = 0x1337 (4919)
        0x00, 0x42,             // 3..4: tracking = 0x0042 (66)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // 5..10: timestamp with nonzero high byte = 0x010203040506
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, // 11..18: order_ref
        'B',                    // 19: buy
        0x00, 0x00, 0x01, 0xF4, // 20..23: shares = 500 (0x1F4)
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', // 24..31: stock
        0x00, 0x17, 0x48, 0x70  // 32..35: price = 1,525,872 ($152.5872)
    };

    MockHandler handler;
    bool ok = ItchParser::parse_record(raw, sizeof(raw), handler);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(handler.last_add_order.has_value());

    const auto& m = *handler.last_add_order;
    EXPECT_EQ(m.header.message_type, 'A');
    EXPECT_EQ(m.header.stock_locate, 0x1337);
    EXPECT_EQ(m.header.tracking_number, 0x0042);
    EXPECT_EQ(m.header.timestamp_ns, 0x010203040506ULL);
    EXPECT_EQ(m.order_ref, 0x1122334455667788ULL);
    EXPECT_EQ(m.side, Side::Buy);
    EXPECT_EQ(m.shares, 500);
    EXPECT_EQ(std::string_view(m.stock.data(), 8), "AAPL    ");
    EXPECT_EQ(m.price, 1525872);
}

// Hand-build raw big-endian bytes for Add Order with MPID ('F')
TEST(ItchParserTest, HandBuiltAddOrderWithMPID) {
    // Length: 40 bytes payload (indices 0..39)
    uint8_t raw[40] = {
        'F',                    // 0: type
        0x1D, 0x1B,             // 1..2: stock_locate = 7451 (SPY)
        0x00, 0x55,             // 3..4: tracking = 85
        0x00, 0x00, 0x99, 0x88, 0x77, 0x66, // 5..10: timestamp
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // 11..18: order_ref
        'S',                    // 19: Sell
        0x00, 0x00, 0x03, 0xE8, // 20..23: shares = 1000
        'S', 'P', 'Y', ' ', ' ', ' ', ' ', ' ', // 24..31: stock
        0x00, 0x31, 0x35, 0xA8, // 32..35: price = 3,225,000 ($322.50)
        'G', 'S', 'C', 'O'      // 36..39: mpid = Goldman Sachs
    };

    MockHandler handler;
    bool ok = ItchParser::parse_record(raw, sizeof(raw), handler);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(handler.last_add_order.has_value());

    const auto& m = *handler.last_add_order;
    EXPECT_EQ(m.header.message_type, 'F');
    EXPECT_EQ(m.header.stock_locate, 7451);
    EXPECT_EQ(m.order_ref, 0x0102030405060708ULL);
    EXPECT_EQ(m.side, Side::Sell);
    EXPECT_EQ(m.shares, 1000);
    EXPECT_EQ(std::string_view(m.stock.data(), 8), "SPY     ");
    EXPECT_EQ(m.price, 3225000);
    EXPECT_EQ(std::string_view(m.mpid.data(), 4), "GSCO");
}

// Hand-build raw big-endian bytes for Order Executed ('E')
TEST(ItchParserTest, HandBuiltOrderExecuted) {
    // Length: 31 bytes payload (indices 0..30)
    uint8_t raw[31] = {
        'E',                    // 0: type
        0x00, 0x0D,             // 1..2: stock_locate = 13 (AAPL)
        0x00, 0x0A,             // 3..4: tracking = 10
        0x00, 0x00, 0x11, 0x22, 0x33, 0x44, // 5..10: timestamp
        0xFE, 0xED, 0xFA, 0xCE, 0x11, 0x22, 0x33, 0x44, // 11..18: order_ref
        0x00, 0x00, 0x00, 0xFA, // 19..22: executed_shares = 250 (0xFA)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08  // 23..30: match_number
    };

    MockHandler handler;
    bool ok = ItchParser::parse_record(raw, sizeof(raw), handler);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(handler.last_order_executed.has_value());

    const auto& m = *handler.last_order_executed;
    EXPECT_EQ(m.header.message_type, 'E');
    EXPECT_EQ(m.header.stock_locate, 13);
    EXPECT_EQ(m.order_ref, 0xFEEDFACE11223344ULL);
    EXPECT_EQ(m.executed_shares, 250);
    EXPECT_EQ(m.match_number, 0x0102030405060708ULL);
}

// Hand-build raw big-endian bytes for Order Replace ('U')
TEST(ItchParserTest, HandBuiltOrderReplace) {
    // Length: 35 bytes payload (indices 0..34)
    uint8_t raw[35] = {
        'U',                    // 0: type
        0x20, 0x01,             // 1..2: stock_locate = 0x2001 (8193)
        0x00, 0x10,             // 3..4: tracking
        0x00, 0x00, 0x05, 0x00, 0x00, 0x01, // 5..10: timestamp
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, // 11..18: orig_ref
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, // 19..26: new_ref
        0x00, 0x00, 0x03, 0xE8, // 27..30: shares = 1000 (0x3E8)
        0x00, 0x16, 0xE3, 0x60  // 31..34: price = 1500000 ($150.00)
    };

    MockHandler handler;
    bool ok = ItchParser::parse_record(raw, sizeof(raw), handler);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(handler.last_order_replace.has_value());

    const auto& m = *handler.last_order_replace;
    EXPECT_EQ(m.header.message_type, 'U');
    EXPECT_EQ(m.header.stock_locate, 0x2001);
    EXPECT_EQ(m.original_order_ref, 0xAABBCCDDEEFF0011ULL);
    EXPECT_EQ(m.new_order_ref, 0x1122334455667788ULL);
    EXPECT_EQ(m.shares, 1000);
    EXPECT_EQ(m.price, 1500000);
}

// Hand-build raw big-endian bytes for Order Executed with Price ('C')
TEST(ItchParserTest, HandBuiltOrderExecutedWithPrice) {
    // Length: 36 bytes payload (indices 0..35)
    uint8_t raw[36] = {
        'C',                    // 0: type
        0x00, 0x0D,             // 1..2: stock_locate = 13 (AAPL)
        0x00, 0x01,             // 3..4: tracking
        0x00, 0x00, 0x10, 0x20, 0x30, 0x40, // 5..10: timestamp
        0xCA, 0xFE, 0xBA, 0xBE, 0x12, 0x34, 0x56, 0x78, // 11..18: order_ref
        0x00, 0x00, 0x00, 0x64, // 19..22: executed_shares = 100
        0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, // 23..30: match_number
        'Y',                    // 31: printable
        0x00, 0x1E, 0x84, 0x80  // 32..35: price = 2000000 ($200.00)
    };

    MockHandler handler;
    bool ok = ItchParser::parse_record(raw, sizeof(raw), handler);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(handler.last_executed_with_price.has_value());

    const auto& m = *handler.last_executed_with_price;
    EXPECT_EQ(m.header.message_type, 'C');
    EXPECT_EQ(m.order_ref, 0xCAFEBABE12345678ULL);
    EXPECT_EQ(m.executed_shares, 100);
    EXPECT_EQ(m.match_number, 0x00000000DEADBEEFULL);
    EXPECT_EQ(m.printable, 'Y');
    EXPECT_EQ(m.execution_price, 2000000);
}

// Hand-build raw big-endian bytes for Delete ('D') and Cancel ('X')
TEST(ItchParserTest, HandBuiltDeleteAndCancel) {
    // Delete (19 bytes, indices 0..18)
    uint8_t raw_d[19] = {
        'D',
        0x1D, 0x1B, // locate
        0x00, 0x02, // tracking
        0x00, 0x00, 0x12, 0x34, 0x56, 0x78, // timestamp
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
    };
    MockHandler handler_d;
    EXPECT_TRUE(ItchParser::parse_record(raw_d, sizeof(raw_d), handler_d));
    ASSERT_TRUE(handler_d.last_order_delete.has_value());
    EXPECT_EQ(handler_d.last_order_delete->order_ref, 0x0123456789ABCDEFULL);

    // Cancel (23 bytes, indices 0..22)
    uint8_t raw_x[23] = {
        'X',
        0x1D, 0x1B,
        0x00, 0x03,
        0x00, 0x00, 0x12, 0x34, 0x56, 0x79,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0x00, 0x00, 0x00, 0x32 // 50 canceled shares
    };
    MockHandler handler_x;
    EXPECT_TRUE(ItchParser::parse_record(raw_x, sizeof(raw_x), handler_x));
    ASSERT_TRUE(handler_x.last_order_cancel.has_value());
    EXPECT_EQ(handler_x.last_order_cancel->order_ref, 0x0123456789ABCDEFULL);
    EXPECT_EQ(handler_x.last_order_cancel->canceled_shares, 50);
}

// Truncated buffer test: must return false gracefully and not read out-of-bounds
TEST(ItchParserTest, TruncatedBufferHandling) {
    MockHandler handler;
    uint8_t short_header[10] = {'A', 0, 1, 0, 2, 0, 0, 0, 0, 0};
    EXPECT_FALSE(ItchParser::parse_record(short_header, sizeof(short_header), handler));

    uint8_t truncated_a[30] = {'A'}; // 'A' requires 35 bytes
    EXPECT_FALSE(ItchParser::parse_record(truncated_a, sizeof(truncated_a), handler));

    uint8_t truncated_u[25] = {'U'}; // 'U' requires 34 bytes
    EXPECT_FALSE(ItchParser::parse_record(truncated_u, sizeof(truncated_u), handler));

    uint8_t truncated_c[32] = {'C'}; // 'C' requires 35 bytes
    EXPECT_FALSE(ItchParser::parse_record(truncated_c, sizeof(truncated_c), handler));
}

// Unknown message type test: must succeed (skip cleanly using length prefix)
TEST(ItchParserTest, UnknownMessageTypeSkipping) {
    MockHandler handler;
    // Type 'Z' (unknown message), length 25
    uint8_t unknown_msg[25] = {
        'Z',
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    };
    EXPECT_TRUE(ItchParser::parse_record(unknown_msg, sizeof(unknown_msg), handler));
}

// Reference Book Locked vs Crossed Book Invariant Test
TEST(ReferenceOrderBookTest, LockedAndCrossedBookTracking) {
    ReferenceOrderBook book("INVARIANT_TEST", 1);

    // 1. Add Bid at $100.00 (1,000,000)
    book.add_order(1, Side::Buy, 100, 1000000);
    // 2. Add Ask at $101.00 (1,010,000)
    book.add_order(2, Side::Sell, 100, 1010000);

    auto s1 = book.get_stats();
    EXPECT_EQ(s1.crossed_book_events, 0);
    EXPECT_EQ(s1.locked_book_events, 0);

    // 3. Add Bid at $101.00 -> Locked Book (best bid == best ask)
    book.add_order(3, Side::Buy, 100, 1010000);
    auto s2 = book.get_stats();
    EXPECT_EQ(s2.crossed_book_events, 0);
    EXPECT_EQ(s2.locked_book_events, 1);

    // 4. Add Bid at $102.00 -> Crossed Book (best bid > best ask)
    book.add_order(4, Side::Buy, 100, 1020000);
    auto s3 = book.get_stats();
    EXPECT_EQ(s3.crossed_book_events, 1);
    EXPECT_EQ(s3.locked_book_events, 1);
}

// Reference Book Separated Adds vs Replaces Counting
TEST(ReferenceOrderBookTest, SeparatedAddsAndReplaces) {
    ReferenceOrderBook book("COUNTS_TEST", 2);

    // 1 Add
    book.add_order(101, Side::Buy, 100, 1000000);
    EXPECT_EQ(book.get_stats().total_adds, 1);
    EXPECT_EQ(book.get_stats().total_replaces, 0);

    // 1 Replace (old order 101 -> new order 102)
    book.replace_order(101, 102, 150, 1005000);
    EXPECT_EQ(book.get_stats().total_adds, 1);
    EXPECT_EQ(book.get_stats().total_replaces, 1);

    // Verify resting order count is 1
    EXPECT_EQ(book.resting_orders_count(), 1);
}

// Replay integration test on the CI test slice
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
    std::string path = "data/fixture_3symbols.bin.gz";
    if (!std::filesystem::exists(path)) {
        path = "../" + path;
    }
    size_t count = ItchParser::parse_file(path, handler);
    EXPECT_EQ(count, 16869);
    EXPECT_GT(handler.adds, 0);
    EXPECT_GT(handler.deletes, 0);

    auto stats = handler.book.get_stats();
    EXPECT_GT(stats.peak_resting_orders, 0);
    EXPECT_GT(stats.peak_bid_levels, 0);
    EXPECT_GT(stats.peak_ask_levels, 0);
}
