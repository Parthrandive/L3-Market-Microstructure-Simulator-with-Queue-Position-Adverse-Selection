#include <gtest/gtest.h>
#include <filesystem>
#include <vector>
#include "orderbook/order_book.hpp"
#include "orderbook/itch_parser.hpp"
#include "reference_book.hpp"

using namespace l3;

TEST(PoolTest, AllocationAndRecycling) {
    Pool<OrderNode> pool(8, "TestPool");
    EXPECT_EQ(pool.capacity(), 8);
    EXPECT_EQ(pool.allocated_count(), 0);

    uint32_t idx0 = pool.allocate();
    uint32_t idx1 = pool.allocate();
    uint32_t idx2 = pool.allocate();

    EXPECT_EQ(pool.allocated_count(), 3);
    EXPECT_EQ(pool.peak_allocated(), 3);

    pool[idx0].order_ref = 1001;
    pool[idx1].order_ref = 1002;
    pool[idx2].order_ref = 1003;

    // Free idx1 (middle)
    pool.deallocate(idx1);
    EXPECT_EQ(pool.allocated_count(), 2);

    // Free idx0
    pool.deallocate(idx0);
    EXPECT_EQ(pool.allocated_count(), 1);

    // Next allocations should recycle recently freed nodes (LIFO free list)
    uint32_t recycled0 = pool.allocate();
    EXPECT_EQ(recycled0, idx0);

    uint32_t recycled1 = pool.allocate();
    EXPECT_EQ(recycled1, idx1);

    EXPECT_EQ(pool.allocated_count(), 3);
}

TEST(OrderIdMapTest, BasicOperations) {
    OrderIdMap map(16);
    EXPECT_EQ(map.size(), 0);
    EXPECT_EQ(map.capacity(), 16);

    EXPECT_TRUE(map.insert(100, 1));
    EXPECT_TRUE(map.insert(200, 2));
    EXPECT_TRUE(map.insert(300, 3));

    EXPECT_EQ(map.size(), 3);
    EXPECT_EQ(map.find(100), 1);
    EXPECT_EQ(map.find(200), 2);
    EXPECT_EQ(map.find(300), 3);
    EXPECT_EQ(map.find(999), NULL_INDEX);

    // Erase an entry
    EXPECT_TRUE(map.erase(200));
    EXPECT_EQ(map.size(), 2);
    EXPECT_EQ(map.find(200), NULL_INDEX);
    EXPECT_EQ(map.find(100), 1);
    EXPECT_EQ(map.find(300), 3);

    // Erase non-existent
    EXPECT_FALSE(map.erase(999));
    EXPECT_EQ(map.size(), 2);
}

TEST(OrderIdMapTest, BackwardShiftDeletionInCollisionChain) {
    // Table capacity 16. Find keys that collide to bucket 5
    OrderIdMap map(16);
    std::vector<uint64_t> colliding_keys;

    for (uint64_t k = 1; colliding_keys.size() < 4; ++k) {
        if ((OrderIdMap::hash_key(k) & 15) == 5) {
            colliding_keys.push_back(k);
        }
    }

    ASSERT_EQ(colliding_keys.size(), 4);

    // Insert all 4 keys. They will probe into slots 5, 6, 7, 8
    for (size_t i = 0; i < colliding_keys.size(); ++i) {
        map.insert(colliding_keys[i], static_cast<uint32_t>(i + 10));
    }

    EXPECT_EQ(map.size(), 4);
    for (size_t i = 0; i < colliding_keys.size(); ++i) {
        EXPECT_EQ(map.find(colliding_keys[i]), static_cast<uint32_t>(i + 10));
    }

    // Delete the second key in the chain (in the middle of probe chain)
    EXPECT_TRUE(map.erase(colliding_keys[1]));
    EXPECT_EQ(map.size(), 3);
    EXPECT_EQ(map.find(colliding_keys[1]), NULL_INDEX);

    // Crucial: keys after the hole MUST still be found via backward shift
    EXPECT_EQ(map.find(colliding_keys[0]), 10);
    EXPECT_EQ(map.find(colliding_keys[2]), 12);
    EXPECT_EQ(map.find(colliding_keys[3]), 13);

    // Delete head of remaining chain
    EXPECT_TRUE(map.erase(colliding_keys[0]));
    EXPECT_EQ(map.size(), 2);
    EXPECT_EQ(map.find(colliding_keys[0]), NULL_INDEX);
    EXPECT_EQ(map.find(colliding_keys[2]), 12);
    EXPECT_EQ(map.find(colliding_keys[3]), 13);
}

TEST(OrderIdMapTest, CyclicWrapAroundCollisionAndDeletion) {
    // Capacity 16. Find keys that hash to bucket 15 (last bucket)
    OrderIdMap map(16);
    std::vector<uint64_t> keys;

    for (uint64_t k = 1; keys.size() < 4; ++k) {
        if ((OrderIdMap::hash_key(k) & 15) == 15) {
            keys.push_back(k);
        }
    }

    // Keys will occupy slot 15, then wrap to 0, 1, 2
    for (size_t i = 0; i < keys.size(); ++i) {
        map.insert(keys[i], static_cast<uint32_t>(i + 50));
    }

    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(map.find(keys[i]), static_cast<uint32_t>(i + 50));
    }

    // Erase key at slot 15 (before wrap-around)
    EXPECT_TRUE(map.erase(keys[0]));
    EXPECT_EQ(map.find(keys[0]), NULL_INDEX);

    // Wrapped-around keys in slots 0, 1, 2 must still be found!
    EXPECT_EQ(map.find(keys[1]), 51);
    EXPECT_EQ(map.find(keys[2]), 52);
    EXPECT_EQ(map.find(keys[3]), 53);

    // Erase wrapped element
    EXPECT_TRUE(map.erase(keys[2]));
    EXPECT_EQ(map.find(keys[1]), 51);
    EXPECT_EQ(map.find(keys[2]), NULL_INDEX);
    EXPECT_EQ(map.find(keys[3]), 53);
}

TEST(PriceLevelTest, SortedBidsAscendingWithBackTouch) {
    SortedBids bids;
    EXPECT_TRUE(bids.empty());
    EXPECT_EQ(bids.best_price(), 0);

    bids.insert(10000, 1);
    bids.insert(10500, 2);
    bids.insert(10250, 3);

    // For bids, highest price is best bid at back()
    EXPECT_EQ(bids.size(), 3);
    EXPECT_EQ(bids.best_price(), 10500);
    EXPECT_EQ(bids.best_level_idx(), 2);

    bids.pop_best();
    EXPECT_EQ(bids.size(), 2);
    EXPECT_EQ(bids.best_price(), 10250);
    EXPECT_EQ(bids.best_level_idx(), 3);

    // Remove middle or specific price
    EXPECT_TRUE(bids.remove(10000));
    EXPECT_EQ(bids.size(), 1);
    EXPECT_EQ(bids.best_price(), 10250);
}

TEST(PriceLevelTest, SortedAsksDescendingWithBackTouch) {
    SortedAsks asks;
    EXPECT_TRUE(asks.empty());
    EXPECT_EQ(asks.best_price(), 0);

    asks.insert(10500, 1);
    asks.insert(10200, 2);
    asks.insert(10350, 3);

    // For asks, lowest price is best ask at back()
    EXPECT_EQ(asks.size(), 3);
    EXPECT_EQ(asks.best_price(), 10200);
    EXPECT_EQ(asks.best_level_idx(), 2);

    asks.pop_best();
    EXPECT_EQ(asks.size(), 2);
    EXPECT_EQ(asks.best_price(), 10350);
    EXPECT_EQ(asks.best_level_idx(), 3);

    asks.pop_best();
    EXPECT_EQ(asks.size(), 1);
    EXPECT_EQ(asks.best_price(), 10500);
}

TEST(OrderBookTest, BasicLifecycleAndBBO) {
    OrderBook book("SPY", 7451);

    EXPECT_EQ(book.resting_orders_count(), 0);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());

    // Add bids
    book.add_order(1, Side::Buy, 100, 3200000); // $320.00
    book.add_order(2, Side::Buy, 200, 3205000); // $320.50
    book.add_order(3, Side::Buy, 150, 3205000); // $320.50

    // Add asks
    book.add_order(4, Side::Sell, 500, 3210000); // $321.00
    book.add_order(5, Side::Sell, 300, 3215000); // $321.50

    EXPECT_EQ(book.resting_orders_count(), 5);
    EXPECT_EQ(book.bid_levels_count(), 2);
    EXPECT_EQ(book.ask_levels_count(), 2);

    auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->first, 3205000);
    EXPECT_EQ(bb->second, 350); // 200 + 150

    auto ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->first, 3210000);
    EXPECT_EQ(ba->second, 500);

    // Partial execute on order 2 (head of best bid)
    book.execute_order(2, 50);
    bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->second, 300); // 150 + 150

    // Full execute remaining of order 2
    book.execute_order(2, 150);
    bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->second, 150); // only order 3 remains at 3205000
    EXPECT_EQ(book.resting_orders_count(), 4);

    // Cancel order 3 completely -> level 3205000 empties
    book.cancel_order(3, 150);
    bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->first, 3200000); // falls back to next level
    EXPECT_EQ(bb->second, 100);
    EXPECT_EQ(book.bid_levels_count(), 1);

    // Replace order 4 (best ask) to lower price $320.80
    book.replace_order(4, 6, 400, 3208000);
    ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->first, 3208000);
    EXPECT_EQ(ba->second, 400);

    // Delete order 5
    book.delete_order(5);
    EXPECT_EQ(book.ask_levels_count(), 1);
    EXPECT_EQ(book.resting_orders_count(), 2); // order 1 and order 6
}

// Differential testing: Full parity verification between ReferenceOrderBook and OrderBook
TEST(OrderBookTest, DifferentialParityOnTestSlice) {
    std::string path = "data/fixture_3symbols.bin.gz";
    if (!std::filesystem::exists(path)) {
        path = "../" + path;
    }

    struct Pair {
        ReferenceOrderBook ref;
        OrderBook arena;
    };

    // Locates: 7451 = SPY, 13 = AAPL, 6428 = NVDA
    std::unordered_map<uint16_t, Pair> books;
    books.emplace(7451, Pair{ReferenceOrderBook("SPY", 7451), OrderBook("SPY", 7451)});
    books.emplace(13,   Pair{ReferenceOrderBook("AAPL", 13),  OrderBook("AAPL", 13)});
    books.emplace(6428, Pair{ReferenceOrderBook("NVDA", 6428), OrderBook("NVDA", 6428)});

    auto check_parity = [](const Pair& pair, size_t msg_idx) {
        const auto& ref = pair.ref;
        const auto& arena = pair.arena;

        ASSERT_EQ(ref.resting_orders_count(), arena.resting_orders_count())
            << "Mismatch in resting orders at message " << msg_idx;
        ASSERT_EQ(ref.bid_levels_count(), arena.bid_levels_count())
            << "Mismatch in bid levels at message " << msg_idx;
        ASSERT_EQ(ref.ask_levels_count(), arena.ask_levels_count())
            << "Mismatch in ask levels at message " << msg_idx;

        auto ref_bb = ref.best_bid();
        auto arena_bb = arena.best_bid();
        ASSERT_EQ(ref_bb.has_value(), arena_bb.has_value())
            << "Mismatch in best_bid presence at message " << msg_idx;
        if (ref_bb.has_value()) {
            ASSERT_EQ(ref_bb->first, arena_bb->first)
                << "Mismatch in best_bid price at message " << msg_idx;
            ASSERT_EQ(ref_bb->second, arena_bb->second)
                << "Mismatch in best_bid shares at message " << msg_idx;
        }

        auto ref_ba = ref.best_ask();
        auto arena_ba = arena.best_ask();
        ASSERT_EQ(ref_ba.has_value(), arena_ba.has_value())
            << "Mismatch in best_ask presence at message " << msg_idx;
        if (ref_ba.has_value()) {
            ASSERT_EQ(ref_ba->first, arena_ba->first)
                << "Mismatch in best_ask price at message " << msg_idx;
            ASSERT_EQ(ref_ba->second, arena_ba->second)
                << "Mismatch in best_ask shares at message " << msg_idx;
        }
    };

    struct DifferentialHandler {
        std::unordered_map<uint16_t, Pair>& books_ref;
        const decltype(check_parity)& checker;
        size_t msg_count{0};

        void on_system_event(const SystemEventMsg&) {}
        void on_stock_directory(const StockDirectoryMsg&) {}

        void on_add_order(const AddOrderMsg& m) {
            msg_count++;
            auto it = books_ref.find(m.header.stock_locate);
            if (it != books_ref.end()) {
                it->second.ref.add_order(m.order_ref, m.side, m.shares, m.price);
                it->second.arena.add_order(m.order_ref, m.side, m.shares, m.price, msg_count);
                checker(it->second, msg_count);
            }
        }

        void on_order_executed(const OrderExecutedMsg& m) {
            msg_count++;
            auto it = books_ref.find(m.header.stock_locate);
            if (it != books_ref.end()) {
                it->second.ref.execute_order(m.order_ref, m.executed_shares);
                it->second.arena.execute_order(m.order_ref, m.executed_shares);
                checker(it->second, msg_count);
            }
        }

        void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
            msg_count++;
            auto it = books_ref.find(m.header.stock_locate);
            if (it != books_ref.end()) {
                it->second.ref.execute_order(m.order_ref, m.executed_shares);
                it->second.arena.execute_order(m.order_ref, m.executed_shares);
                checker(it->second, msg_count);
            }
        }

        void on_order_cancel(const OrderCancelMsg& m) {
            msg_count++;
            auto it = books_ref.find(m.header.stock_locate);
            if (it != books_ref.end()) {
                it->second.ref.cancel_order(m.order_ref, m.canceled_shares);
                it->second.arena.cancel_order(m.order_ref, m.canceled_shares);
                checker(it->second, msg_count);
            }
        }

        void on_order_delete(const OrderDeleteMsg& m) {
            msg_count++;
            auto it = books_ref.find(m.header.stock_locate);
            if (it != books_ref.end()) {
                it->second.ref.delete_order(m.order_ref);
                it->second.arena.delete_order(m.order_ref);
                checker(it->second, msg_count);
            }
        }

        void on_order_replace(const OrderReplaceMsg& m) {
            msg_count++;
            auto it = books_ref.find(m.header.stock_locate);
            if (it != books_ref.end()) {
                it->second.ref.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price);
                it->second.arena.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price, msg_count);
                checker(it->second, msg_count);
            }
        }

        void on_trade(const TradeMsg&) {}
    };

    DifferentialHandler handler{books, check_parity, 0};
    size_t count = ItchParser::parse_file(path, handler);
    EXPECT_EQ(count, 16869);
    EXPECT_GT(handler.msg_count, 0);

    // Final checks on each book
    for (const auto& [loc, pair] : books) {
        check_parity(pair, 999999);
    }
}
