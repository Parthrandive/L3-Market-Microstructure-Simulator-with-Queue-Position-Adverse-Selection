#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include "orderbook/itch_parser.hpp"
#include "orderbook/reference_book.hpp"

namespace l3 {

class ReferenceDispatcher {
public:
    ReferenceDispatcher() = default;

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

        locate_to_symbol_[msg.header.stock_locate] = sym;
        if (books_.find(msg.header.stock_locate) == books_.end()) {
            books_.emplace(msg.header.stock_locate, ReferenceOrderBook(sym, msg.header.stock_locate));
        }
    }

    void on_add_order(const AddOrderMsg& msg) {
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.add_order(msg.order_ref, msg.side, msg.shares, msg.price);
        }
    }

    void on_order_executed(const OrderExecutedMsg& msg) {
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.execute_order(msg.order_ref, msg.executed_shares);
        }
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& msg) {
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.execute_order(msg.order_ref, msg.executed_shares);
        }
    }

    void on_order_cancel(const OrderCancelMsg& msg) {
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.cancel_order(msg.order_ref, msg.canceled_shares);
        }
    }

    void on_order_delete(const OrderDeleteMsg& msg) {
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.delete_order(msg.order_ref);
        }
    }

    void on_order_replace(const OrderReplaceMsg& msg) {
        auto it = books_.find(msg.header.stock_locate);
        if (it != books_.end()) {
            it->second.replace_order(msg.original_order_ref, msg.new_order_ref, msg.shares, msg.price);
        }
    }

    void on_trade(const TradeMsg&) {
        trades_++;
    }

    void print_summary() {
        std::cout << "\n======================================================\n";
        std::cout << "          REFERENCE ORDER BOOK EMPIRICAL METRICS       \n";
        std::cout << "======================================================\n";
        std::cout << "System Events: " << system_events_ << ", Non-displayed Trades: " << trades_ << "\n";

        size_t total_peak_orders = 0;
        size_t max_symbol_orders = 0;
        size_t max_price_levels = 0;

        for (auto& [loc, book] : books_) {
            book.print_stats();
            auto s = book.get_stats();
            total_peak_orders += s.peak_resting_orders;
            max_symbol_orders = std::max(max_symbol_orders, s.peak_resting_orders);
            max_price_levels = std::max(max_price_levels, s.peak_bid_levels + s.peak_ask_levels);
        }

        std::cout << "\n======================================================\n";
        std::cout << "RECOMMENDED ARENA & HASH TABLE SIZING (FROM MEASUREMENTS):\n";
        std::cout << "  Single Symbol Max Peak Orders: " << max_symbol_orders << "\n";
        std::cout << "  Combined Peak Resting Orders:  " << total_peak_orders << "\n";
        std::cout << "  Peak Active Price Levels:      " << max_price_levels << "\n";
        std::cout << "======================================================\n";
    }

    std::unordered_map<uint16_t, ReferenceOrderBook>& books() { return books_; }

private:
    std::unordered_map<uint16_t, std::string> locate_to_symbol_;
    std::unordered_map<uint16_t, ReferenceOrderBook> books_;
    size_t system_events_{0};
    size_t trades_{0};
};

} // namespace l3

int main(int argc, char* argv[]) {
    std::string fixture_path = (argc > 1) ? argv[1] : "data/fixture_3symbols.bin.gz";
    std::cout << "Running Reference Order Book on: " << fixture_path << "\n";

    l3::ReferenceDispatcher dispatcher;
    auto t0 = std::chrono::high_resolution_clock::now();
    size_t msgs = l3::ItchParser::parse_file(fixture_path, dispatcher);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "\nProcessed " << msgs << " messages in " << dur_ms << " ms ("
              << (dur_ms > 0 ? (msgs * 1000 / dur_ms) : 0) << " msg/s)\n";

    dispatcher.print_summary();
    return 0;
}
