#pragma once

#include <cstdint>
#include <limits>
#include "orderbook/types.hpp"

namespace l3 {

// Sentinel representing an invalid or null pool index
inline constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

// OrderNode represents an active resting order in the book.
// Memory footprint: EXACTLY 32 bytes to ensure two nodes fit cleanly
// into a single 64-byte L1 CPU cache line.
struct alignas(8) OrderNode {
    uint64_t order_ref{0};         // 8 bytes: Unique exchange order reference number
    uint32_t price{0};             // 4 bytes: Fixed-point price (4 decimal places)
    uint32_t shares{0};            // 4 bytes: Current resting share quantity
    uint32_t next_idx{NULL_INDEX}; // 4 bytes: Next order index in FIFO price-level queue (or next free)
    uint32_t prev_idx{NULL_INDEX}; // 4 bytes: Previous order index in FIFO price-level queue
    uint32_t level_idx{NULL_INDEX};// 4 bytes: Stable index to parent PriceLevel in PriceLevelPool
    Side side{Side::Buy};          // 1 byte:  Buy (0) or Sell (1)
    uint8_t flags{0};              // 1 byte:  Bit flags for execution state
    uint8_t padding[2]{0, 0};      // 2 bytes: Explicit padding to 32 bytes

    [[nodiscard]] bool is_valid() const noexcept {
        return order_ref != 0;
    }
};

// Strict compile-time invariant ensuring cache-line efficiency
static_assert(sizeof(OrderNode) == 32, "OrderNode must be exactly 32 bytes");
static_assert(alignof(OrderNode) >= 4, "OrderNode must be at least 4-byte aligned");

} // namespace l3
