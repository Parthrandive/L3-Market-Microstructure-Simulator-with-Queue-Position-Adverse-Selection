#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include "orderbook/itch_parser.hpp"
#include "orderbook/order_book.hpp"
#include "reference_book.hpp"

namespace l3 {

struct SymbolPair {
    std::string symbol;
    uint16_t locate;
    ReferenceOrderBook ref;
    OrderBook arena;
};

class DifferentialDispatcher {
public:
    DifferentialDispatcher() = default;

    void on_system_event(const SystemEventMsg& msg) {
        system_events_++;
        if (msg.event_code == 'Q') {
            std::cout << "[Event] Market Hours Open (09:30 ET) at " << msg.header.timestamp_ns << " ns\n";
        } else if (msg.event_code == 'M') {
            std::cout << "[Event] Market Hours Close (16:00 ET) at " << msg.header.timestamp_ns << " ns\n";
        }
    }

    void on_stock_directory(const StockDirectoryMsg& msg) {
        std::string sym(msg.stock.data(), 8);
        while (!sym.empty() && sym.back() == ' ') sym.pop_back();

        if (books_.find(msg.header.stock_locate) == books_.end()) {
            OrderBookConfig cfg{
                .order_pool_capacity = 65536,  // > 2x AAPL peak
                .level_pool_capacity = 8192,   // > 2x AAPL peak levels
                .order_map_capacity = 131072   // Low load factor (< 0.25)
            };
            books_.emplace(msg.header.stock_locate, SymbolPair{
                .symbol = sym,
                .locate = msg.header.stock_locate,
                .ref = ReferenceOrderBook(sym, msg.header.stock_locate),
                .arena = OrderBook(sym, msg.header.stock_locate, cfg)
            });
        }
    }

    void on_add_order(const AddOrderMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.ref.add_order(msg.order_ref, msg.side, msg.shares, msg.price);
            it->second.arena.add_order(msg.order_ref, msg.side, msg.shares, msg.price, msg_count_);
            check_parity(it->second, msg_count_);
        }
    }

    void on_order_executed(const OrderExecutedMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.ref.execute_order(msg.order_ref, msg.executed_shares);
            it->second.arena.execute_order(msg.order_ref, msg.executed_shares);
            check_parity(it->second, msg_count_);
        }
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.ref.execute_order(msg.order_ref, msg.executed_shares);
            it->second.arena.execute_order(msg.order_ref, msg.executed_shares);
            check_parity(it->second, msg_count_);
        }
    }

    void on_order_cancel(const OrderCancelMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.ref.cancel_order(msg.order_ref, msg.canceled_shares);
            it->second.arena.cancel_order(msg.order_ref, msg.canceled_shares);
            check_parity(it->second, msg_count_);
        }
    }

    void on_order_delete(const OrderDeleteMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.ref.delete_order(msg.order_ref);
            it->second.arena.delete_order(msg.order_ref);
            check_parity(it->second, msg_count_);
        }
    }

    void on_order_replace(const OrderReplaceMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.ref.replace_order(msg.original_order_ref, msg.new_order_ref, msg.shares, msg.price);
            it->second.arena.replace_order(msg.original_order_ref, msg.new_order_ref, msg.shares, msg.price, msg_count_);
            check_parity(it->second, msg_count_);
        }
    }

    void on_trade(const TradeMsg&) {
        trades_++;
    }

    void check_parity(const SymbolPair& pair, size_t msg_idx) {
        const auto& ref = pair.ref;
        const auto& arena = pair.arena;

        // 1. Mandatory per-message check: BBO must match identically after EVERY single message
        auto ref_bb = ref.best_bid();
        auto arena_bb = arena.best_bid();
        if (ref_bb != arena_bb) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol << " Best Bid mismatch!\n";
            if (ref_bb) std::cerr << "  Ref:   Price=" << ref_bb->first << " Shares=" << ref_bb->second << "\n";
            else        std::cerr << "  Ref:   None\n";
            if (arena_bb) std::cerr << "  Arena: Price=" << arena_bb->first << " Shares=" << arena_bb->second << "\n";
            else          std::cerr << "  Arena: None\n";
            std::abort();
        }

        auto ref_ba = ref.best_ask();
        auto arena_ba = arena.best_ask();
        if (ref_ba != arena_ba) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol << " Best Ask mismatch!\n";
            if (ref_ba) std::cerr << "  Ref:   Price=" << ref_ba->first << " Shares=" << ref_ba->second << "\n";
            else        std::cerr << "  Ref:   None\n";
            if (arena_ba) std::cerr << "  Arena: Price=" << arena_ba->first << " Shares=" << arena_ba->second << "\n";
            else          std::cerr << "  Arena: None\n";
            std::abort();
        }

        // 2. Full deep checkpoint assertion every 500 messages:
        // Validates order counts, price level counts, AND exact FIFO queue sequences per level
        if (msg_idx % 500 != 0) {
            return;
        }

        if (ref.resting_orders_count() != arena.resting_orders_count()) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol
                      << " resting orders: ref=" << ref.resting_orders_count()
                      << " vs arena=" << arena.resting_orders_count() << "\n";
            std::abort();
        }

        if (ref.bid_levels_count() != arena.bid_levels_count()) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol
                      << " bid levels: ref=" << ref.bid_levels_count()
                      << " vs arena=" << arena.bid_levels_count() << "\n";
            std::abort();
        }

        if (ref.ask_levels_count() != arena.ask_levels_count()) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol
                      << " ask levels: ref=" << ref.ask_levels_count()
                      << " vs arena=" << arena.ask_levels_count() << "\n";
            std::abort();
        }

        // Deep queue order comparison: exact sequence of order refs at each level
        auto ref_bid_queues = ref.get_all_bid_queues();
        auto arena_bid_queues = arena.get_all_bid_queues();
        if (ref_bid_queues != arena_bid_queues) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol
                      << " Bid FIFO queue order mismatch!\n";
            std::abort();
        }

        auto ref_ask_queues = ref.get_all_ask_queues();
        auto arena_ask_queues = arena.get_all_ask_queues();
        if (ref_ask_queues != arena_ask_queues) {
            std::cerr << "\n[MISMATCH at msg " << msg_idx << "] " << pair.symbol
                      << " Ask FIFO queue order mismatch!\n";
            std::abort();
        }

        checks_performed_++;
    }

    void print_summary() {
        std::cout << "\n======================================================\n";
        std::cout << "     FULL-DAY DIFFERENTIAL PARITY VERIFICATION REPORT  \n";
        std::cout << "======================================================\n";
        std::cout << "Total messages processed: " << msg_count_ << "\n";
        std::cout << "Periodic state check validations: " << checks_performed_ << "\n";
        std::cout << "System Events: " << system_events_ << ", Non-displayed Trades: " << trades_ << "\n\n";

        for (auto& [loc, pair] : books_) {
            std::cout << "Ticker: " << pair.symbol << " (Locate: " << loc << ")\n";
            std::cout << "  Resting Orders at EOD:       Ref=" << pair.ref.resting_orders_count()
                      << " | Arena=" << pair.arena.resting_orders_count() << "\n";
            std::cout << "  Bid Levels at EOD:           Ref=" << pair.ref.bid_levels_count()
                      << " | Arena=" << pair.arena.bid_levels_count() << "\n";
            std::cout << "  Ask Levels at EOD:           Ref=" << pair.ref.ask_levels_count()
                      << " | Arena=" << pair.arena.ask_levels_count() << "\n";

            const auto& arena = pair.arena;
            std::cout << "  Arena Order Pool Peak:       " << arena.order_pool().peak_allocated()
                      << " / " << arena.order_pool().capacity() << " ("
                      << std::fixed << std::setprecision(2)
                      << (100.0 * arena.order_pool().peak_allocated() / arena.order_pool().capacity()) << "%)\n";
            std::cout << "  Arena Level Pool Peak:       " << arena.level_pool().peak_allocated()
                      << " / " << arena.level_pool().capacity() << " ("
                      << (100.0 * arena.level_pool().peak_allocated() / arena.level_pool().capacity()) << "%)\n";
            std::cout << "  Arena OrderIdMap Max Probes: " << arena.order_map().max_probe_length()
                      << " (Avg per insert: " << arena.order_map().avg_probes_per_insert() << ")\n";
            std::cout << "  Arena OrderIdMap Load Factor:" << (arena.order_map().load_factor() * 100.0) << "%\n";

            // End-of-day final parity assertion
            if (pair.ref.resting_orders_count() != pair.arena.resting_orders_count() ||
                pair.ref.best_bid() != pair.arena.best_bid() ||
                pair.ref.best_ask() != pair.arena.best_ask() ||
                pair.ref.get_all_bid_queues() != pair.arena.get_all_bid_queues() ||
                pair.ref.get_all_ask_queues() != pair.arena.get_all_ask_queues()) {
                std::cerr << "\n[FATAL] Final EOD parity check failed for " << pair.symbol << "\n";
                std::abort();
            }
            std::cout << "  -> Final State Parity: PASS (Bit-for-bit identical)\n\n";
        }
        std::cout << "======================================================\n";
    }

private:
    std::unordered_map<uint16_t, SymbolPair> books_;
    size_t msg_count_{0};
    size_t checks_performed_{0};
    size_t system_events_{0};
    size_t trades_{0};
};

} // namespace l3

int main(int argc, char* argv[]) {
    std::string fixture_path = (argc > 1) ? argv[1] : "data/fixture_3symbols_full.bin.gz";
    std::cout << "Starting Differential Parity Test against Reference Book...\n";
    std::cout << "Target Fixture: " << fixture_path << "\n";

    l3::DifferentialDispatcher dispatcher;
    auto t0 = std::chrono::high_resolution_clock::now();
    size_t msgs = l3::ItchParser::parse_file(fixture_path, dispatcher);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "\nProcessed " << msgs << " messages in " << dur_ms << " ms ("
              << (dur_ms > 0 ? (msgs * 1000 / dur_ms) : 0) << " msg/s)\n";

    dispatcher.print_summary();
    return 0;
}
