#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <iostream>
#include <cassert>
#include <algorithm>
#include "orderbook/types.hpp"
#include "orderbook/itch_messages.hpp"

namespace l3 {

struct LevelInfo {
    uint32_t total_shares{0};
    uint32_t order_count{0};
};

struct ReferenceOrder {
    uint64_t order_ref;
    Side side;
    uint32_t price;
    uint32_t shares;
};

struct BookStats {
    std::string symbol{};
    uint16_t stock_locate{0};
    size_t peak_resting_orders{0};
    size_t peak_bid_levels{0};
    size_t peak_ask_levels{0};
    size_t total_distinct_prices_seen{0};
    size_t total_adds{0};       // Only 'A' and 'F' add messages
    size_t total_replaces{0};   // 'U' replace messages
    size_t total_executes{0};   // 'E' and 'C' executions
    size_t total_cancels{0};    // 'X' partial cancels
    size_t total_deletes{0};    // 'D' deletes
    size_t crossed_book_events{0}; // best_bid > best_ask
    size_t locked_book_events{0};  // best_bid == best_ask
    size_t missing_order_events{0};
    size_t negative_shares_events{0};
};

class ReferenceOrderBook {
public:
    explicit ReferenceOrderBook(std::string symbol = "", uint16_t locate = 0)
        : symbol_(std::move(symbol)), locate_(locate) {
        stats_.symbol = symbol_;
        stats_.stock_locate = locate_;
    }

    // Handles 'A' and 'F'
    void add_order(uint64_t order_ref, Side side, uint32_t shares, uint32_t price) {
        stats_.total_adds++;
        insert_order_internal(order_ref, side, shares, price);
    }

    void execute_order(uint64_t order_ref, uint32_t executed_shares) {
        stats_.total_executes++;
        auto it = orders_.find(order_ref);
        if (it == orders_.end()) {
            stats_.missing_order_events++;
            return;
        }

        ReferenceOrder& ord = it->second;
        if (executed_shares >= ord.shares) {
            remove_from_level(ord.side, ord.price, ord.shares);
            orders_.erase(it);
        } else {
            ord.shares -= executed_shares;
            reduce_level_shares(ord.side, ord.price, executed_shares);
        }
    }

    void cancel_order(uint64_t order_ref, uint32_t canceled_shares) {
        stats_.total_cancels++;
        auto it = orders_.find(order_ref);
        if (it == orders_.end()) {
            stats_.missing_order_events++;
            return;
        }

        ReferenceOrder& ord = it->second;
        if (canceled_shares >= ord.shares) {
            remove_from_level(ord.side, ord.price, ord.shares);
            orders_.erase(it);
        } else {
            ord.shares -= canceled_shares;
            reduce_level_shares(ord.side, ord.price, canceled_shares);
        }
    }

    void delete_order(uint64_t order_ref) {
        stats_.total_deletes++;
        auto it = orders_.find(order_ref);
        if (it == orders_.end()) {
            stats_.missing_order_events++;
            return;
        }

        ReferenceOrder& ord = it->second;
        remove_from_level(ord.side, ord.price, ord.shares);
        orders_.erase(it);
    }

    // Handles 'U' replace
    void replace_order(uint64_t orig_ref, uint64_t new_ref, uint32_t new_shares, uint32_t new_price) {
        stats_.total_replaces++;
        auto it = orders_.find(orig_ref);
        Side side = Side::Buy;
        if (it != orders_.end()) {
            side = it->second.side;
            remove_from_level(side, it->second.price, it->second.shares);
            orders_.erase(it);
        } else {
            stats_.missing_order_events++;
        }

        insert_order_internal(new_ref, side, new_shares, new_price);
    }

    // Book state queries
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        const auto& [price, lvl] = *bids_.begin();
        return std::make_pair(price, lvl.total_shares);
    }

    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        const auto& [price, lvl] = *asks_.begin();
        return std::make_pair(price, lvl.total_shares);
    }

    [[nodiscard]] size_t resting_orders_count() const noexcept { return orders_.size(); }
    [[nodiscard]] size_t bid_levels_count() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_levels_count() const noexcept { return asks_.size(); }
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] uint16_t stock_locate() const noexcept { return locate_; }

    [[nodiscard]] BookStats get_stats() {
        stats_.total_distinct_prices_seen = distinct_prices_.size();
        return stats_;
    }

    void print_stats() {
        auto s = get_stats();
        std::cout << "\n=== Book Stats for " << (symbol_.empty() ? "Locate " + std::to_string(locate_) : symbol_) << " ===\n";
        std::cout << "  Peak resting orders:        " << s.peak_resting_orders << "\n";
        std::cout << "  Peak active bid levels:     " << s.peak_bid_levels << "\n";
        std::cout << "  Peak active ask levels:     " << s.peak_ask_levels << "\n";
        std::cout << "  Total distinct prices seen: " << s.total_distinct_prices_seen << "\n";
        std::cout << "  Adds ('A' + 'F'):           " << s.total_adds << "\n";
        std::cout << "  Replaces ('U'):             " << s.total_replaces << "\n";
        std::cout << "  Deletes ('D'):              " << s.total_deletes << "\n";
        std::cout << "  Executions ('E' + 'C'):     " << s.total_executes << "\n";
        std::cout << "  Cancels ('X'):              " << s.total_cancels << "\n";
        std::cout << "  Crossed book events:        " << s.crossed_book_events << "\n";
        std::cout << "  Locked book events:         " << s.locked_book_events << "\n";
        std::cout << "  Missing order lookups:      " << s.missing_order_events << "\n";
        std::cout << "  Current resting orders:     " << orders_.size() << "\n";
        if (auto bb = best_bid()) std::cout << "  Final Best Bid:             $" << (bb->first / 10000.0) << " (" << bb->second << " shares)\n";
        if (auto ba = best_ask()) std::cout << "  Final Best Ask:             $" << (ba->first / 10000.0) << " (" << ba->second << " shares)\n";
    }

private:
    void insert_order_internal(uint64_t order_ref, Side side, uint32_t shares, uint32_t price) {
        distinct_prices_.insert(price);

        orders_[order_ref] = ReferenceOrder{
            .order_ref = order_ref,
            .side = side,
            .price = price,
            .shares = shares
        };

        if (side == Side::Buy) {
            auto& lvl = bids_[price];
            lvl.total_shares += shares;
            lvl.order_count++;
            stats_.peak_bid_levels = std::max(stats_.peak_bid_levels, bids_.size());
        } else {
            auto& lvl = asks_[price];
            lvl.total_shares += shares;
            lvl.order_count++;
            stats_.peak_ask_levels = std::max(stats_.peak_ask_levels, asks_.size());
        }

        stats_.peak_resting_orders = std::max(stats_.peak_resting_orders, orders_.size());
        check_crossed_and_locked_book();
    }

    void reduce_level_shares(Side side, uint32_t price, uint32_t shares) {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it != bids_.end()) {
                if (shares >= it->second.total_shares) {
                    it->second.total_shares = 0;
                } else {
                    it->second.total_shares -= shares;
                }
            }
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) {
                if (shares >= it->second.total_shares) {
                    it->second.total_shares = 0;
                } else {
                    it->second.total_shares -= shares;
                }
            }
        }
    }

    void remove_from_level(Side side, uint32_t price, uint32_t shares) {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it != bids_.end()) {
                if (shares >= it->second.total_shares) {
                    it->second.total_shares = 0;
                } else {
                    it->second.total_shares -= shares;
                }
                if (it->second.order_count > 0) it->second.order_count--;
                if (it->second.order_count == 0) bids_.erase(it);
            }
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) {
                if (shares >= it->second.total_shares) {
                    it->second.total_shares = 0;
                } else {
                    it->second.total_shares -= shares;
                }
                if (it->second.order_count > 0) it->second.order_count--;
                if (it->second.order_count == 0) asks_.erase(it);
            }
        }
    }

    void check_crossed_and_locked_book() {
        if (!bids_.empty() && !asks_.empty()) {
            uint32_t bb = bids_.begin()->first;
            uint32_t ba = asks_.begin()->first;
            if (bb > ba) {
                stats_.crossed_book_events++;
            } else if (bb == ba) {
                stats_.locked_book_events++;
            }
        }
    }

    std::string symbol_{};
    uint16_t locate_{0};
    BookStats stats_{};

    std::unordered_map<uint64_t, ReferenceOrder> orders_{};
    std::map<uint32_t, LevelInfo, std::greater<uint32_t>> bids_{};
    std::map<uint32_t, LevelInfo, std::less<uint32_t>> asks_{};
    std::unordered_set<uint32_t> distinct_prices_{};
};

} // namespace l3
