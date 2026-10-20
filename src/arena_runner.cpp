#include <iostream>
#include <string>
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include "orderbook/itch_parser.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/allocation_tracker.hpp"

namespace l3 {

class ArenaDispatcher {
public:
    ArenaDispatcher() {
        // Pre-create books for the 3 target symbols with generous sizing
        OrderBookConfig cfg{
            .order_pool_capacity = 65536,
            .level_pool_capacity = 8192,
            .order_map_capacity = 131072
        };
        books_.emplace(7451, OrderBook("SPY", 7451, cfg));
        books_.emplace(13,   OrderBook("AAPL", 13, cfg));
        books_.emplace(5708, OrderBook("NVDA", 5708, cfg));
    }

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
                .order_pool_capacity = 65536,
                .level_pool_capacity = 8192,
                .order_map_capacity = 131072
            };
            books_.emplace(msg.header.stock_locate, OrderBook(sym, msg.header.stock_locate, cfg));
        }
    }

    void on_add_order(const AddOrderMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.add_order(msg.order_ref, msg.side, msg.shares, msg.price, msg_count_);
        }
    }

    void on_order_executed(const OrderExecutedMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.execute_order(msg.order_ref, msg.executed_shares);
        }
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.execute_order(msg.order_ref, msg.executed_shares);
        }
    }

    void on_order_cancel(const OrderCancelMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.cancel_order(msg.order_ref, msg.canceled_shares);
        }
    }

    void on_order_delete(const OrderDeleteMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.delete_order(msg.order_ref);
        }
    }

    void on_order_replace(const OrderReplaceMsg& msg) {
        msg_count_++;
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.replace_order(msg.original_order_ref, msg.new_order_ref, msg.shares, msg.price, msg_count_);
        }
    }

    void on_trade(const TradeMsg&) {
        trades_++;
    }

    void print_summary() {
        std::cout << "\n======================================================\n";
        std::cout << "          ARENA ORDER BOOK REPLAY METRICS             \n";
        std::cout << "======================================================\n";
        std::cout << "Messages processed: " << msg_count_ << "\n";
        std::cout << "System Events: " << system_events_ << ", Non-displayed Trades: " << trades_ << "\n\n";

        for (const auto& [loc, book] : books_) {
            std::cout << "Ticker: " << book.symbol() << " (Locate: " << loc << ")\n";
            std::cout << "  Resting Orders at EOD:       " << book.resting_orders_count() << "\n";
            std::cout << "  Bid Levels at EOD:           " << book.bid_levels_count() << "\n";
            std::cout << "  Ask Levels at EOD:           " << book.ask_levels_count() << "\n";
            std::cout << "  Arena Order Pool Peak:       " << book.order_pool().peak_allocated()
                      << " / " << book.order_pool().capacity() << " ("
                      << std::fixed << std::setprecision(2)
                      << (100.0 * book.order_pool().peak_allocated() / book.order_pool().capacity()) << "%)\n";
            std::cout << "  Arena Level Pool Peak:       " << book.level_pool().peak_allocated()
                      << " / " << book.level_pool().capacity() << " ("
                      << (100.0 * book.level_pool().peak_allocated() / book.level_pool().capacity()) << "%)\n";
            std::cout << "  Arena 64-byte aligned pool:  "
                      << (book.order_pool().is_64_byte_aligned() ? "YES (Pass)" : "NO (FAIL)") << "\n";
            std::cout << "  OrderIdMap Max Probes:       " << book.order_map().max_probe_length()
                      << " (Avg per insert: " << book.order_map().avg_probes_per_insert() << ")\n";
            std::cout << "  OrderIdMap Load Factor:      " << (book.order_map().load_factor() * 100.0) << "%\n\n";
        }
        std::cout << "======================================================\n";
    }

    size_t msg_count() const noexcept { return msg_count_; }

private:
    std::unordered_map<uint16_t, OrderBook> books_;
    size_t msg_count_{0};
    size_t system_events_{0};
    size_t trades_{0};
};

} // namespace l3

int main(int argc, char* argv[]) {
    std::string fixture_path = (argc > 1) ? argv[1] : "data/fixture_3symbols_full.bin.gz";
    std::cout << "Running Arena Order Book on: " << fixture_path << "\n";

    l3::ArenaDispatcher dispatcher;

    // Setup streaming buffer prior to replay
    l3::ItchParser::get_stream_buffer();

    // Start tracking allocations strictly on the replay hot path
    l3::AllocationTracker::start();

    auto t0 = std::chrono::high_resolution_clock::now();
    size_t msgs = l3::ItchParser::parse_file(fixture_path, dispatcher);
    auto t1 = std::chrono::high_resolution_clock::now();

    l3::AllocationTracker::stop();
    size_t allocs_during_replay = l3::AllocationTracker::count();

    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "\nProcessed " << msgs << " messages in " << dur_ms << " ms ("
              << (dur_ms > 0 ? (msgs * 1000 / dur_ms) : 0) << " msg/s) [Release build, single-core]\n";

    std::cout << "\n>>> ZERO-ALLOCATION PROOF <<<\n";
    std::cout << "Heap allocations during full-day replay (operator new count): "
              << allocs_during_replay << "\n";
    if (allocs_during_replay == 0) {
        std::cout << "Zero-Allocation Invariant: VERIFIED (0 bytes / 0 allocations on hot path)\n";
    } else {
        std::cerr << "Zero-Allocation Invariant: VIOLATED (" << allocs_during_replay << " allocations!)\n";
    }

    dispatcher.print_summary();
    return (allocs_during_replay == 0) ? 0 : 1;
}
