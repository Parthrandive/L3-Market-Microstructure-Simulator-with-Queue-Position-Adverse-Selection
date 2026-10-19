#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include "orderbook/types.hpp"
#include "orderbook/order_node.hpp"
#include "orderbook/pool.hpp"

namespace l3 {

// PriceLevel represents an aggregate price level containing a FIFO queue of orders.
// Allocated in a stable PriceLevelPool so level_idx never changes during its lifetime.
struct PriceLevel {
    uint32_t price{0};              // 4 bytes: price (4 decimals)
    uint32_t total_shares{0};       // 4 bytes: sum of resting shares across all orders
    uint32_t order_count{0};        // 4 bytes: count of resting orders at this level
    uint32_t head_order_idx{NULL_INDEX}; // 4 bytes: FIFO head (first to be filled)
    uint32_t tail_order_idx{NULL_INDEX}; // 4 bytes: FIFO tail (newest order joined)
    Side side{Side::Buy};           // 1 byte:  Buy or Sell
    uint8_t padding[3]{0, 0, 0};    // 3 bytes padding
};

// Compact 8-byte reference stored in the sorted active price-level vectors
struct alignas(8) LevelRef {
    uint32_t price{0};
    uint32_t level_idx{NULL_INDEX};

    bool operator==(const LevelRef& other) const noexcept {
        return price == other.price;
    }
};
static_assert(sizeof(LevelRef) == 8, "LevelRef must be exactly 8 bytes");

// Sorted price levels vector with best price at the back for O(1) touch operations
// Bids: Sorted ascending by price  -> Best Bid (highest price) is at levels_.back()
// Asks: Sorted descending by price -> Best Ask (lowest price) is at levels_.back()
template<Side S>
class SortedLevels {
public:
    SortedLevels() {
        levels_.reserve(2048);
    }

    void clear() noexcept {
        levels_.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return levels_.empty();
    }

    [[nodiscard]] size_t size() const noexcept {
        return levels_.size();
    }

    // O(1) top-of-book touch access
    [[nodiscard]] uint32_t best_level_idx() const noexcept {
        if (levels_.empty()) return NULL_INDEX;
        return levels_.back().level_idx;
    }

    [[nodiscard]] uint32_t best_price() const noexcept {
        if (levels_.empty()) return 0;
        return levels_.back().price;
    }

    // O(1) top-of-book level exhaustion
    void pop_best() noexcept {
        if (!levels_.empty()) {
            levels_.pop_back();
        }
    }

    // Finds the level_idx for a given price via binary search (std::lower_bound)
    [[nodiscard]] uint32_t find(uint32_t price) const noexcept {
        auto it = find_iterator(price);
        if (it != levels_.end() && it->price == price) {
            return it->level_idx;
        }
        return NULL_INDEX;
    }

    // Inserts a new active price level into the sorted array
    void insert(uint32_t price, uint32_t level_idx) {
        if constexpr (S == Side::Buy) {
            // Ascending: target goes before the first element with price >= target
            auto it = std::lower_bound(levels_.begin(), levels_.end(), price,
                [](const LevelRef& item, uint32_t val) {
                    return item.price < val;
                });
            levels_.insert(it, LevelRef{price, level_idx});
        } else {
            // Descending: target goes before the first element with price <= target
            auto it = std::lower_bound(levels_.begin(), levels_.end(), price,
                [](const LevelRef& item, uint32_t val) {
                    return item.price > val;
                });
            levels_.insert(it, LevelRef{price, level_idx});
        }
    }

    // Removes an emptied price level
    bool remove(uint32_t price) noexcept {
        if (levels_.empty()) return false;

        // Fast path: if removing the best level at touch, O(1) pop_back()!
        if (levels_.back().price == price) {
            levels_.pop_back();
            return true;
        }

        auto it = find_iterator(price);
        if (it != levels_.end() && it->price == price) {
            levels_.erase(it);
            return true;
        }
        return false;
    }

    [[nodiscard]] const std::vector<LevelRef>& levels() const noexcept {
        return levels_;
    }

private:
    auto find_iterator(uint32_t price) const noexcept {
        if constexpr (S == Side::Buy) {
            return std::lower_bound(levels_.begin(), levels_.end(), price,
                [](const LevelRef& item, uint32_t val) {
                    return item.price < val;
                });
        } else {
            return std::lower_bound(levels_.begin(), levels_.end(), price,
                [](const LevelRef& item, uint32_t val) {
                    return item.price > val;
                });
        }
    }

    std::vector<LevelRef> levels_{};
};

using SortedBids = SortedLevels<Side::Buy>;
using SortedAsks = SortedLevels<Side::Sell>;

} // namespace l3
