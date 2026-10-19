#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <array>
#include <concepts>
#include <span>
#include <zlib.h>
#include "orderbook/itch_messages.hpp"

namespace l3 {

// Endian-safe helpers: standard memcpy + bswap
// Guaranteed no undefined behavior or unaligned pointer dereference (UBSan clean)
inline uint16_t read_u16_be(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

inline uint32_t read_u32_be(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

inline uint64_t read_u64_be(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

inline uint64_t read_u48_be(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 40) |
           (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) |
           (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) << 8)  |
           (static_cast<uint64_t>(p[5]));
}

inline MessageHeader parse_header(const uint8_t* p) noexcept {
    return MessageHeader{
        .message_type = static_cast<char>(p[0]),
        .stock_locate = read_u16_be(p + 1),
        .tracking_number = read_u16_be(p + 3),
        .timestamp_ns = read_u48_be(p + 5)
    };
}

// Concept for zero-overhead static dispatch handler
template<typename T>
concept ItchHandler = requires(T& h,
                               const SystemEventMsg& s,
                               const StockDirectoryMsg& r,
                               const AddOrderMsg& a,
                               const OrderExecutedMsg& e,
                               const OrderExecutedWithPriceMsg& c,
                               const OrderCancelMsg& x,
                               const OrderDeleteMsg& d,
                               const OrderReplaceMsg& u,
                               const TradeMsg& p) {
    { h.on_system_event(s) };
    { h.on_stock_directory(r) };
    { h.on_add_order(a) };
    { h.on_order_executed(e) };
    { h.on_order_executed_with_price(c) };
    { h.on_order_cancel(x) };
    { h.on_order_delete(d) };
    { h.on_order_replace(u) };
    { h.on_trade(p) };
};

class ItchParser {
public:
    template<typename Handler>
    static bool parse_record(const uint8_t* p, uint16_t len, Handler& handler) {
        if (len < 11) [[unlikely]] return false;
        const char mtype = static_cast<char>(p[0]);
        const MessageHeader header = parse_header(p);

        switch (mtype) {
            case 'A': {
                if (len < 36) return false;
                AddOrderMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11),
                    .side = (p[19] == 'B') ? Side::Buy : Side::Sell,
                    .shares = read_u32_be(p + 20),
                    .stock = {},
                    .price = read_u32_be(p + 32),
                    .mpid = {' ', ' ', ' ', ' '}
                };
                std::memcpy(msg.stock.data(), p + 24, 8);
                handler.on_add_order(msg);
                return true;
            }
            case 'F': {
                if (len < 40) return false;
                AddOrderMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11),
                    .side = (p[19] == 'B') ? Side::Buy : Side::Sell,
                    .shares = read_u32_be(p + 20),
                    .stock = {},
                    .price = read_u32_be(p + 32),
                    .mpid = {}
                };
                std::memcpy(msg.stock.data(), p + 24, 8);
                std::memcpy(msg.mpid.data(), p + 36, 4);
                handler.on_add_order(msg);
                return true;
            }
            case 'E': {
                if (len < 31) return false;
                OrderExecutedMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11),
                    .executed_shares = read_u32_be(p + 19),
                    .match_number = read_u64_be(p + 23)
                };
                handler.on_order_executed(msg);
                return true;
            }
            case 'C': {
                if (len < 36) return false;
                OrderExecutedWithPriceMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11),
                    .executed_shares = read_u32_be(p + 19),
                    .match_number = read_u64_be(p + 23),
                    .printable = static_cast<char>(p[31]),
                    .execution_price = read_u32_be(p + 32)
                };
                handler.on_order_executed_with_price(msg);
                return true;
            }
            case 'X': {
                if (len < 23) return false;
                OrderCancelMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11),
                    .canceled_shares = read_u32_be(p + 19)
                };
                handler.on_order_cancel(msg);
                return true;
            }
            case 'D': {
                if (len < 19) return false;
                OrderDeleteMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11)
                };
                handler.on_order_delete(msg);
                return true;
            }
            case 'U': {
                if (len < 35) return false;
                OrderReplaceMsg msg{
                    .header = header,
                    .original_order_ref = read_u64_be(p + 11),
                    .new_order_ref = read_u64_be(p + 19),
                    .shares = read_u32_be(p + 27),
                    .price = read_u32_be(p + 31)
                };
                handler.on_order_replace(msg);
                return true;
            }
            case 'P': {
                if (len < 44) return false;
                TradeMsg msg{
                    .header = header,
                    .order_ref = read_u64_be(p + 11),
                    .side = (p[19] == 'B') ? Side::Buy : Side::Sell,
                    .shares = read_u32_be(p + 20),
                    .stock = {},
                    .price = read_u32_be(p + 32),
                    .match_number = read_u64_be(p + 36),
                    .is_cross = false,
                    .cross_type = ' '
                };
                std::memcpy(msg.stock.data(), p + 24, 8);
                handler.on_trade(msg);
                return true;
            }
            case 'Q': {
                if (len < 40) return false;
                TradeMsg msg{
                    .header = header,
                    .order_ref = 0,
                    .side = Side::Buy,
                    .shares = static_cast<uint32_t>(read_u64_be(p + 11)),
                    .stock = {},
                    .price = read_u32_be(p + 27),
                    .match_number = read_u64_be(p + 31),
                    .is_cross = true,
                    .cross_type = static_cast<char>(p[39])
                };
                std::memcpy(msg.stock.data(), p + 19, 8);
                handler.on_trade(msg);
                return true;
            }
            case 'S': {
                if (len < 12) return false;
                SystemEventMsg msg{
                    .header = header,
                    .event_code = static_cast<char>(p[11])
                };
                handler.on_system_event(msg);
                return true;
            }
            case 'R': {
                if (len < 39) return false;
                StockDirectoryMsg msg{
                    .header = header,
                    .stock = {},
                    .market_category = static_cast<char>(p[19]),
                    .financial_status = static_cast<char>(p[20]),
                    .round_lot_size = read_u32_be(p + 21),
                    .round_lots_only = static_cast<char>(p[25]),
                    .issue_classification = static_cast<char>(p[26]),
                    .issue_subtype = {static_cast<char>(p[27]), static_cast<char>(p[28])},
                    .authenticity = static_cast<char>(p[29]),
                    .short_sale_threshold = static_cast<char>(p[30]),
                    .ipo_flag = static_cast<char>(p[31]),
                    .luld_tier = static_cast<char>(p[32]),
                    .etp_flag = static_cast<char>(p[33]),
                    .etp_leverage_factor = read_u32_be(p + 34),
                    .inverse_indicator = (len >= 39) ? static_cast<char>(p[38]) : ' '
                };
                std::memcpy(msg.stock.data(), p + 11, 8);
                handler.on_stock_directory(msg);
                return true;
            }
            default:
                // Other ITCH messages ('H', 'Y', 'L', 'I', etc.) can be ignored or handled
                return true;
        }
    }

    // High-performance streaming reader for gzipped or raw ITCH files
    template<typename Handler>
    static size_t parse_file(const std::string& path, Handler& handler, size_t max_messages = 0) {
        gzFile gz = gzopen(path.c_str(), "rb");
        if (!gz) return 0;

        constexpr size_t BUF_CAP = 8 * 1024 * 1024; // 8 MB read buffer
        std::vector<uint8_t> buffer(BUF_CAP);
        size_t buf_len = 0;
        size_t messages_parsed = 0;

        while (true) {
            int bytes_read = gzread(gz, buffer.data() + buf_len, static_cast<unsigned int>(BUF_CAP - buf_len));
            if (bytes_read <= 0) break;
            buf_len += static_cast<size_t>(bytes_read);

            size_t pos = 0;
            while (pos + 2 <= buf_len) {
                const uint16_t msg_len = (static_cast<uint16_t>(buffer[pos]) << 8) | buffer[pos + 1];
                const size_t rec_end = pos + 2 + msg_len;
                if (rec_end > buf_len) break;

                parse_record(buffer.data() + pos + 2, msg_len, handler);
                messages_parsed++;
                pos = rec_end;

                if (max_messages > 0 && messages_parsed >= max_messages) {
                    gzclose(gz);
                    return messages_parsed;
                }
            }

            if (pos > 0) {
                std::memmove(buffer.data(), buffer.data() + pos, buf_len - pos);
                buf_len -= pos;
            }
        }

        gzclose(gz);
        return messages_parsed;
    }
};

} // namespace l3
