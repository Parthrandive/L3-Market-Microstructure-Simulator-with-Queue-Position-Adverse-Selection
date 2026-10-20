#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <utility>
#include <map>
#include <vector>
#include "orderbook/types.hpp"
#include "orderbook/order_node.hpp"
#include "orderbook/pool.hpp"
#include "orderbook/order_id_map.hpp"
#include "orderbook/price_level.hpp"

namespace l3 {

struct OrderBookConfig {
    size_t order_pool_capacity{65536};     // Headroom for peak resting orders
    size_t level_pool_capacity{8192};      // Headroom for peak distinct levels
    size_t order_map_capacity{131072};     // Power-of-2 capacity for order-ID hash table
};

enum class LevelMutation : uint8_t {
    None = 0,         // Mutated order inside existing price level
    LevelCreated = 1, // New price level created and inserted into sorted array
    LevelRemoved = 2  // Empty price level removed from sorted array
};

// High-performance L3 Limit Order Book with zero allocations on the hot path.
// Backed by a 32-byte OrderNode pool, stable PriceLevel pool, open-addressed
// order-ID hash table, and tail-touch sorted level arrays.
class OrderBook {
public:
    using Config = OrderBookConfig;

    explicit OrderBook(std::string symbol = "", uint16_t locate = 0, OrderBookConfig config = {})
        : symbol_(std::move(symbol)),
          locate_(locate),
          config_(config),
          order_pool_(config.order_pool_capacity, "OrderNodePool (" + symbol_ + ")"),
          level_pool_(config.level_pool_capacity, "PriceLevelPool (" + symbol_ + ")"),
          order_map_(config.order_map_capacity),
          bids_(config.level_pool_capacity),
          asks_(config.level_pool_capacity) {}

    void clear() {
        order_pool_.reset();
        level_pool_.reset();
        order_map_.reset();
        bids_.clear();
        asks_.clear();
    }

    // Handles 'A' and 'F' add messages
    LevelMutation add_order(uint64_t order_ref, Side side, uint32_t shares, uint32_t price, size_t msg_idx = 0) {
        uint32_t node_idx = order_pool_.allocate(msg_idx);
        uint32_t level_idx = (side == Side::Buy) ? bids_.find(price) : asks_.find(price);
        bool level_created = (level_idx == NULL_INDEX);

        if (level_created) {
            level_idx = level_pool_.allocate(msg_idx);
            PriceLevel& lvl = level_pool_[level_idx];
            lvl = PriceLevel{
                .price = price,
                .total_shares = 0,
                .order_count = 0,
                .head_order_idx = NULL_INDEX,
                .tail_order_idx = NULL_INDEX,
                .side = side,
                .padding = {0, 0, 0}
            };

            if (side == Side::Buy) {
                bids_.insert(price, level_idx);
            } else {
                asks_.insert(price, level_idx);
            }
        }

        PriceLevel& level = level_pool_[level_idx];
        OrderNode& node = order_pool_[node_idx];
        node = OrderNode{
            .order_ref = order_ref,
            .price = price,
            .shares = shares,
            .next_idx = NULL_INDEX,
            .prev_idx = level.tail_order_idx,
            .level_idx = level_idx,
            .side = side,
            .flags = 0,
            .padding = {0, 0}
        };

        // Append to tail of FIFO queue in price level
        if (level.tail_order_idx == NULL_INDEX) {
            level.head_order_idx = node_idx;
            level.tail_order_idx = node_idx;
        } else {
            order_pool_[level.tail_order_idx].next_idx = node_idx;
            level.tail_order_idx = node_idx;
        }

        level.total_shares += shares;
        level.order_count++;

        order_map_.insert(order_ref, node_idx, msg_idx);
        return level_created ? LevelMutation::LevelCreated : LevelMutation::None;
    }

    // Handles 'E' and 'C' executions
    LevelMutation execute_order(uint64_t order_ref, uint32_t executed_shares) {
        uint32_t node_idx = order_map_.find(order_ref);
        if (node_idx == NULL_INDEX) [[unlikely]] return LevelMutation::None;

        OrderNode& node = order_pool_[node_idx];
        if (executed_shares >= node.shares) {
            bool rem = remove_order_internal(node_idx, order_ref);
            return rem ? LevelMutation::LevelRemoved : LevelMutation::None;
        } else {
            node.shares -= executed_shares;
            level_pool_[node.level_idx].total_shares -= executed_shares;
            return LevelMutation::None;
        }
    }

    // Handles 'X' partial cancels
    LevelMutation cancel_order(uint64_t order_ref, uint32_t canceled_shares) {
        uint32_t node_idx = order_map_.find(order_ref);
        if (node_idx == NULL_INDEX) [[unlikely]] return LevelMutation::None;

        OrderNode& node = order_pool_[node_idx];
        if (canceled_shares >= node.shares) {
            bool rem = remove_order_internal(node_idx, order_ref);
            return rem ? LevelMutation::LevelRemoved : LevelMutation::None;
        } else {
            node.shares -= canceled_shares;
            level_pool_[node.level_idx].total_shares -= canceled_shares;
            return LevelMutation::None;
        }
    }

    // Handles 'D' deletes
    LevelMutation delete_order(uint64_t order_ref) {
        uint32_t node_idx = order_map_.find(order_ref);
        if (node_idx == NULL_INDEX) [[unlikely]] return LevelMutation::None;

        bool rem = remove_order_internal(node_idx, order_ref);
        return rem ? LevelMutation::LevelRemoved : LevelMutation::None;
    }

    // Handles 'U' replaces
    LevelMutation replace_order(uint64_t orig_ref, uint64_t new_ref, uint32_t new_shares, uint32_t new_price, size_t msg_idx = 0) {
        uint32_t orig_idx = order_map_.find(orig_ref);
        Side side = Side::Buy;
        bool old_removed = false;
        if (orig_idx != NULL_INDEX) {
            side = order_pool_[orig_idx].side;
            old_removed = remove_order_internal(orig_idx, orig_ref);
        }
        LevelMutation add_mut = add_order(new_ref, side, new_shares, new_price, msg_idx);
        if (add_mut == LevelMutation::LevelCreated) return LevelMutation::LevelCreated;
        if (old_removed) return LevelMutation::LevelRemoved;
        return LevelMutation::None;
    }

    // Book inspection queries
    struct QueuePosition {
        uint32_t shares_ahead{0};
        uint32_t orders_ahead{0};
        uint32_t level_total_shares{0};
        uint32_t order_shares{0};
        uint32_t price{0};
        Side side{Side::Buy};
        bool exists{false};
    };

    [[nodiscard]] QueuePosition get_queue_position(uint64_t order_ref) const noexcept {
        uint32_t node_idx = order_map_.find(order_ref);
        if (node_idx == NULL_INDEX) {
            return QueuePosition{};
        }

        const OrderNode& node = order_pool_[node_idx];
        const PriceLevel& level = level_pool_[node.level_idx];

        uint32_t shares_ahead = 0;
        uint32_t orders_ahead = 0;
        uint32_t curr = node.prev_idx;
        while (curr != NULL_INDEX) {
            shares_ahead += order_pool_[curr].shares;
            orders_ahead++;
            curr = order_pool_[curr].prev_idx;
        }

        return QueuePosition{
            .shares_ahead = shares_ahead,
            .orders_ahead = orders_ahead,
            .level_total_shares = level.total_shares,
            .order_shares = node.shares,
            .price = level.price,
            .side = node.side,
            .exists = true
        };
    }

    [[nodiscard]] bool is_order_ahead(uint64_t candidate_ref, uint64_t target_ref) const noexcept {
        uint32_t target_idx = order_map_.find(target_ref);
        if (target_idx == NULL_INDEX) return false;
        uint32_t cand_idx = order_map_.find(candidate_ref);
        if (cand_idx == NULL_INDEX) return false;

        uint32_t curr = order_pool_[target_idx].prev_idx;
        while (curr != NULL_INDEX) {
            if (curr == cand_idx) return true;
            curr = order_pool_[curr].prev_idx;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> best_bid() const noexcept {
        uint32_t lvl_idx = bids_.best_level_idx();
        if (lvl_idx == NULL_INDEX) return std::nullopt;
        const PriceLevel& lvl = level_pool_[lvl_idx];
        return std::make_pair(lvl.price, lvl.total_shares);
    }

    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> best_ask() const noexcept {
        uint32_t lvl_idx = asks_.best_level_idx();
        if (lvl_idx == NULL_INDEX) return std::nullopt;
        const PriceLevel& lvl = level_pool_[lvl_idx];
        return std::make_pair(lvl.price, lvl.total_shares);
    }

    [[nodiscard]] size_t resting_orders_count() const noexcept {
        return order_pool_.allocated_count();
    }

    [[nodiscard]] size_t bid_levels_count() const noexcept {
        return bids_.size();
    }

    [[nodiscard]] size_t ask_levels_count() const noexcept {
        return asks_.size();
    }

    // Sequence of order_ref in FIFO queue for a specific price level
    [[nodiscard]] std::vector<uint64_t> get_level_queue(Side side, uint32_t price) const {
        std::vector<uint64_t> queue;
        uint32_t lvl_idx = (side == Side::Buy) ? bids_.find(price) : asks_.find(price);
        if (lvl_idx == NULL_INDEX) return queue;

        const PriceLevel& lvl = level_pool_[lvl_idx];
        uint32_t curr = lvl.head_order_idx;
        while (curr != NULL_INDEX) {
            queue.push_back(order_pool_[curr].order_ref);
            curr = order_pool_[curr].next_idx;
        }
        return queue;
    }

    // Returns map of price -> FIFO order_ref sequence for all active bid levels
    [[nodiscard]] std::map<uint32_t, std::vector<uint64_t>> get_all_bid_queues() const {
        std::map<uint32_t, std::vector<uint64_t>> result;
        for (const auto& ref : bids_.levels()) {
            result[ref.price] = get_level_queue(Side::Buy, ref.price);
        }
        return result;
    }

    // Returns map of price -> FIFO order_ref sequence for all active ask levels
    [[nodiscard]] std::map<uint32_t, std::vector<uint64_t>> get_all_ask_queues() const {
        std::map<uint32_t, std::vector<uint64_t>> result;
        for (const auto& ref : asks_.levels()) {
            result[ref.price] = get_level_queue(Side::Sell, ref.price);
        }
        return result;
    }

    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] uint16_t stock_locate() const noexcept { return locate_; }
    [[nodiscard]] const OrderIdMap& order_map() const noexcept { return order_map_; }
    [[nodiscard]] const Pool<OrderNode>& order_pool() const noexcept { return order_pool_; }
    [[nodiscard]] const Pool<PriceLevel>& level_pool() const noexcept { return level_pool_; }

private:
    bool remove_order_internal(uint32_t node_idx, uint64_t order_ref) {
        OrderNode& node = order_pool_[node_idx];
        PriceLevel& level = level_pool_[node.level_idx];

        // Unlink from FIFO linked list
        if (node.prev_idx != NULL_INDEX) {
            order_pool_[node.prev_idx].next_idx = node.next_idx;
        } else {
            level.head_order_idx = node.next_idx;
        }

        if (node.next_idx != NULL_INDEX) {
            order_pool_[node.next_idx].prev_idx = node.prev_idx;
        } else {
            level.tail_order_idx = node.prev_idx;
        }

        level.total_shares = (node.shares >= level.total_shares) ? 0 : (level.total_shares - node.shares);
        level.order_count--;

        uint32_t level_price = level.price;
        Side level_side = level.side;
        uint32_t level_idx = node.level_idx;

        // Free node and erase from map
        order_pool_.deallocate(node_idx);
        order_map_.erase(order_ref);

        // If level is now empty, remove from active sorted levels and deallocate level
        bool level_removed = false;
        if (level.order_count == 0) {
            if (level_side == Side::Buy) {
                bids_.remove(level_price);
            } else {
                asks_.remove(level_price);
            }
            level_pool_.deallocate(level_idx);
            level_removed = true;
        }
        return level_removed;
    }

    std::string symbol_{};
    uint16_t locate_{0};
    Config config_{};

    Pool<OrderNode> order_pool_;
    Pool<PriceLevel> level_pool_;
    OrderIdMap order_map_;
    SortedBids bids_{};
    SortedAsks asks_{};
};

} // namespace l3
