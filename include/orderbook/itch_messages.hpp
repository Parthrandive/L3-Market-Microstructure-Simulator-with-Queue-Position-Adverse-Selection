#pragma once

#include <cstdint>
#include <string_view>
#include <array>
#include "orderbook/types.hpp"

namespace l3 {

struct MessageHeader {
    char message_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;  // Decoded from 6-byte big-endian uint48
};

struct SystemEventMsg {
    MessageHeader header;
    char event_code; // 'O', 'S', 'Q', 'M', 'E', 'C'
};

struct StockDirectoryMsg {
    MessageHeader header;
    std::array<char, 8> stock;
    char market_category;
    char financial_status;
    uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    std::array<char, 2> issue_subtype;
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_tier;
    char etp_flag;
    uint32_t etp_leverage_factor;
    char inverse_indicator;
};

struct AddOrderMsg {
    MessageHeader header;
    uint64_t order_ref;
    Side side;           // Buy / Sell
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price;      // Fixed-point 4 decimals (e.g. 10000 = $1.0000)
    std::array<char, 4> mpid; // Attribution if Type 'F', spaces if Type 'A'
};

struct OrderExecutedMsg {
    MessageHeader header;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderExecutedWithPriceMsg {
    MessageHeader header;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
    char printable;
    uint32_t execution_price;
};

struct OrderCancelMsg {
    MessageHeader header;
    uint64_t order_ref;
    uint32_t canceled_shares;
};

struct OrderDeleteMsg {
    MessageHeader header;
    uint64_t order_ref;
};

struct OrderReplaceMsg {
    MessageHeader header;
    uint64_t original_order_ref;
    uint64_t new_order_ref;
    uint32_t shares;
    uint32_t price;
};

struct TradeMsg {
    MessageHeader header;
    uint64_t order_ref;
    Side side;
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price;
    uint64_t match_number;
    bool is_cross;
    char cross_type;
};

} // namespace l3
