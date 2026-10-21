#pragma once

#include <iostream>
#include <iomanip>
#include <vector>
#include <deque>
#include <queue>
#include <unordered_map>
#include <chrono>
#include <string>
#include <cmath>
#include <random>
#include <algorithm>
#include <sstream>
#include <zlib.h>

#include "orderbook/itch_parser.hpp"
#include "orderbook/order_book.hpp"

namespace l3 {

// Strategy order record tracking execution under both Naive Touch and Queue-Aware L3 models
struct SimOrder {
    uint64_t sim_id{0};
    uint16_t stock_locate{0};
    Side side{Side::Buy};
    uint32_t price{0};           // Limit price (Best Bid at signal time)
    uint32_t target_shares{100}; // Requested size (100 or 500)
    uint32_t shares{100};        // Compatibility alias for target_shares
    uint64_t submit_ts_ns{0};     // Strategy signal timestamp
    uint64_t arrival_ts_ns{0};    // Arrival at matching engine (submit_ts + delta_t)
    uint64_t cancel_ts_ns{0};     // TTL expiration timestamp (arrival_ts + ttl)
    int time_block_id{0};         // 5-minute block id (0..77 for regular trading hours)

    // Initial state upon book entry
    bool entered_book{false};
    uint32_t initial_queue_ahead{0};
    uint64_t arrival_ref_watermark{0};
    double initial_spread_bps{0.0};
    double entry_mid{0.0};

    // Tracking state in matching engine
    uint32_t queue_ahead{0};
    uint32_t executed_shares{0};

    // 1. Model 1: Naive Touch Model (assumes 100% execution upon first trade at or through limit price)
    bool touch_filled{false};
    uint64_t touch_fill_ts{0};
    double touch_fill_mid{0.0};
    double touch_markout_1s_bps{0.0};
    double touch_markout_5s_bps{0.0};
    double touch_markout_30s_bps{0.0};
    bool touch_m1_done{false};
    bool touch_m5_done{false};
    bool touch_m30_done{false};

    // 2. Model 2 / Model 3: Queue-Aware L3 Model (with wire latency delta_t)
    bool l3_first_filled{false};  // Partial or full execution (executed_shares >= 1)
    uint64_t l3_first_fill_ts{0};
    double l3_first_fill_mid{0.0};

    bool l3_full_filled{false};   // Complete execution (executed_shares >= target_shares)
    bool l3_filled{false};        // Compatibility alias for l3_full_filled
    uint64_t l3_fill_ts{0};
    double l3_fill_mid{0.0};

    bool l3_canceled{false};      // Expired at TTL with remaining unfilled shares

    // Markouts for L3 fill (measured from first fill timestamp)
    double l3_markout_1s_bps{0.0};
    double l3_markout_5s_bps{0.0};
    double l3_markout_30s_bps{0.0};
    bool l3_m1_done{false};
    bool l3_m5_done{false};
    bool l3_m30_done{false};
};

struct PendingMarkout {
    size_t order_idx{0};
    uint16_t stock_locate{0};
    uint32_t fill_price{0};
    uint64_t target_ts_ns{0};
    int horizon_sec{0};          // 1, 5, or 30
    bool is_touch_model{false};  // true = touch fill, false = L3 fill
    double fill_mid{0.0};

    // Min-heap ordering: earliest target_ts_ns at top
    bool operator>(const PendingMarkout& other) const noexcept {
        return target_ts_ns > other.target_ts_ns;
    }
};

struct BootstrapCI {
    double point_est{0.0};
    double ci_lo{0.0};
    double ci_hi{0.0};
};

// Summary metrics with accurate statistical labeling
struct FillModelSummary {
    std::string model_name;
    uint64_t delta_t_ns{0};
    size_t total_orders{0};
    size_t total_first_fills{0};
    size_t total_full_fills{0};
    size_t total_fills{0}; // primary reported fills (full-fill for L3, touch for Touch)

    BootstrapCI first_fill_rate_ci{};
    BootstrapCI full_fill_rate_ci{};
    BootstrapCI fill_rate_ci{}; // primary fill rate CI
    BootstrapCI overstatement_ratio_ci{};
    BootstrapCI mean_lead_time_ms_ci{};

    double avg_half_spread_captured_bps{0.0};
    BootstrapCI markout_1s_ci{};
    BootstrapCI markout_5s_ci{};
    BootstrapCI markout_30s_ci{};
    double mid_drift_30s_bps{0.0}; // markout_30s - half_spread

    // Mark-to-mid markout PnL, before fees and exit costs
    double total_pnl_5s_usd{0.0};
    double pnl_per_order_5s_usd{0.0};
    double total_pnl_30s_usd{0.0};
    double pnl_per_order_30s_usd{0.0};
    BootstrapCI pnl_5s_usd_ci{};
    BootstrapCI pnl_30s_usd_ci{};
    BootstrapCI pnl_ratio_5s_ci{}; // Ratio of Touch 5s PnL to L3 5s PnL
};

struct SimulationConfig {
    uint64_t delta_t_ns{0};                  // Wire transit latency
    uint64_t ttl_ns{5'000'000'000ULL};       // TTL in nanoseconds (default 5.0s)
    double obi_threshold{0.0};               // Order Book Imbalance filter (-1.0 for unconditional)
    uint64_t sampling_interval_ns{250'000'000ULL}; // 250 ms sampling rate
    uint32_t order_shares{100};              // Order size in shares (100 or 500)
};

class FillSimulationEngine {
public:
    FillSimulationEngine(std::string path, SimulationConfig config)
        : filepath_(std::move(path)), config_(config) {}

    // Runs simulation and returns completed orders
    std::vector<SimOrder> run() {
        gzFile gz = gzopen(filepath_.c_str(), "rb");
        if (!gz) {
            std::cerr << "Failed to open " << filepath_ << "\n";
            return {};
        }

        std::unordered_map<uint16_t, std::string> locate_to_sym;
        std::unordered_map<uint16_t, OrderBook> books;
        std::unordered_map<uint16_t, double> last_valid_mid;

        std::vector<SimOrder> completed_orders;
        completed_orders.reserve(100000);
        std::deque<SimOrder> in_flight_orders;
        std::vector<size_t> active_resting_indices;
        active_resting_indices.reserve(1024);

        std::priority_queue<PendingMarkout, std::vector<PendingMarkout>, std::greater<PendingMarkout>> pending_markouts;

        constexpr size_t BUF_SIZE = 65536;
        std::vector<uint8_t> buffer(BUF_SIZE);
        size_t leftover = 0;
        uint64_t current_ts_ns = 0;
        uint64_t max_ref_seen = 0;
        uint64_t next_sim_id = 900'000'000'000ULL;
        uint64_t last_order_submit_ts = 0;

        // Strictly regular trading hours: 09:30:00 to 16:00:00 ET
        constexpr uint64_t MARKET_OPEN_NS = 9ULL * 3600ULL * 1'000'000'000ULL + 30ULL * 60ULL * 1'000'000'000ULL;
        constexpr uint64_t MARKET_CLOSE_NS = 16ULL * 3600ULL * 1'000'000'000ULL;

        auto get_current_mid = [&](uint16_t locate) -> double {
            auto b_it = books.find(locate);
            if (b_it != books.end()) {
                auto bb = b_it->second.best_bid();
                auto ba = b_it->second.best_ask();
                if (bb && ba && ba->first > bb->first) {
                    double m = (static_cast<double>(bb->first) + static_cast<double>(ba->first)) / 2.0;
                    last_valid_mid[locate] = m;
                    return m;
                }
            }
            auto l_it = last_valid_mid.find(locate);
            return (l_it != last_valid_mid.end()) ? l_it->second : 0.0;
        };

        auto drain_arrivals = [&]() {
            while (!in_flight_orders.empty() && in_flight_orders.front().arrival_ts_ns <= current_ts_ns) {
                SimOrder ord = in_flight_orders.front();
                in_flight_orders.pop_front();

                auto b_it = books.find(ord.stock_locate);
                if (b_it != books.end()) {
                    ord.entered_book = true;
                    // Initial queue ahead is pure real market volume resting at limit price
                    ord.initial_queue_ahead = b_it->second.get_level_shares(ord.side, ord.price);
                    ord.queue_ahead = ord.initial_queue_ahead;
                    ord.arrival_ref_watermark = max_ref_seen;

                    auto bb = b_it->second.best_bid();
                    auto ba = b_it->second.best_ask();
                    if (bb && ba && ba->first > bb->first) {
                        ord.entry_mid = (static_cast<double>(bb->first) + static_cast<double>(ba->first)) / 2.0;
                        ord.initial_spread_bps = ((static_cast<double>(ba->first) - static_cast<double>(bb->first)) / ord.entry_mid) * 10'000.0;
                    }

                    size_t ord_idx = completed_orders.size();
                    completed_orders.push_back(ord);
                    active_resting_indices.push_back(ord_idx);
                }
            }
        };

        auto process_event = [&](char msg_type, uint16_t locate, uint64_t ts,
                                 uint64_t ref, uint32_t shares, uint32_t price, Side side, uint64_t new_ref) {
            current_ts_ns = ts;

            // Update watermark of highest Nasdaq ITCH order_ref seen so far
            if (msg_type == 'A' || msg_type == 'F' || msg_type == 'E' || msg_type == 'C' || msg_type == 'X' || msg_type == 'D') {
                max_ref_seen = std::max(max_ref_seen, ref);
            } else if (msg_type == 'U') {
                max_ref_seen = std::max(max_ref_seen, std::max(ref, new_ref));
            }

            // 1. Process pending markouts in chronological order
            while (!pending_markouts.empty() && pending_markouts.top().target_ts_ns <= current_ts_ns) {
                PendingMarkout pm = pending_markouts.top();
                pending_markouts.pop();

                double mid = get_current_mid(pm.stock_locate);
                if (mid > 0.0 && pm.fill_price > 0) {
                    double bps = ((mid - static_cast<double>(pm.fill_price)) / static_cast<double>(pm.fill_price)) * 10'000.0;
                    auto& ord = completed_orders[pm.order_idx];
                    if (pm.is_touch_model) {
                        if (pm.horizon_sec == 1) {
                            ord.touch_markout_1s_bps = bps;
                            ord.touch_m1_done = true;
                        } else if (pm.horizon_sec == 5) {
                            ord.touch_markout_5s_bps = bps;
                            ord.touch_m5_done = true;
                        } else if (pm.horizon_sec == 30) {
                            ord.touch_markout_30s_bps = bps;
                            ord.touch_m30_done = true;
                        }
                    } else {
                        if (pm.horizon_sec == 1) {
                            ord.l3_markout_1s_bps = bps;
                            ord.l3_m1_done = true;
                        } else if (pm.horizon_sec == 5) {
                            ord.l3_markout_5s_bps = bps;
                            ord.l3_m5_done = true;
                        } else if (pm.horizon_sec == 30) {
                            ord.l3_markout_30s_bps = bps;
                            ord.l3_m30_done = true;
                        }
                    }
                }
            }

            // 2. In-flight order arrival after simulated wire transit latency
            drain_arrivals();

            // 3. Check resting TTL expiration
            for (auto it = active_resting_indices.begin(); it != active_resting_indices.end();) {
                SimOrder& ord = completed_orders[*it];
                if (current_ts_ns >= ord.cancel_ts_ns) {
                    if (!ord.l3_full_filled) {
                        ord.l3_canceled = true;
                    }
                    it = active_resting_indices.erase(it);
                } else if (ord.touch_filled && ord.l3_full_filled) {
                    it = active_resting_indices.erase(it);
                } else {
                    ++it;
                }
            }

            // 4. Process market event on pure OrderBook
            auto book_it = books.find(locate);
            if (book_it != books.end()) {
                OrderBook& book = book_it->second;

                OrderBook::QueuePosition pos{};
                if (msg_type == 'E' || msg_type == 'C' || msg_type == 'X' || msg_type == 'D' || msg_type == 'U') {
                    pos = book.get_queue_position(ref);
                }

                // If execution: check trades against resting simulated Buy orders
                if ((msg_type == 'E' || msg_type == 'C') && pos.exists && pos.side == Side::Buy) {
                    double fill_mid = get_current_mid(locate);

                    for (size_t ord_idx : active_resting_indices) {
                        SimOrder& ord = completed_orders[ord_idx];
                        if (ord.stock_locate == locate && current_ts_ns <= ord.cancel_ts_ns) {
                            // Case A: Trade through our limit price (pos.price < ord.price)
                            if (pos.price < ord.price) {
                                if (!ord.touch_filled) {
                                    ord.touch_filled = true;
                                    ord.touch_fill_ts = current_ts_ns;
                                    ord.touch_fill_mid = fill_mid;
                                    pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1, true, fill_mid});
                                    pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5, true, fill_mid});
                                    pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 30'000'000'000ULL, 30, true, fill_mid});
                                }
                                if (!ord.l3_full_filled) {
                                    ord.queue_ahead = 0;
                                    uint32_t needed = ord.target_shares - ord.executed_shares;
                                    uint32_t fill_qty = std::min(shares, needed);
                                    ord.executed_shares += fill_qty;

                                    if (!ord.l3_first_filled && ord.executed_shares >= 1) {
                                        ord.l3_first_filled = true;
                                        ord.l3_first_fill_ts = current_ts_ns;
                                        ord.l3_first_fill_mid = fill_mid;
                                        pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1, false, fill_mid});
                                        pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5, false, fill_mid});
                                        pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 30'000'000'000ULL, 30, false, fill_mid});
                                    }
                                    if (ord.executed_shares >= ord.target_shares) {
                                        ord.l3_full_filled = true;
                                        ord.l3_filled = true;
                                        ord.l3_fill_ts = current_ts_ns;
                                        ord.l3_fill_mid = fill_mid;
                                    }
                                }
                            }
                            // Case B: Trade at our limit price (pos.price == ord.price)
                            else if (pos.price == ord.price) {
                                if (!ord.touch_filled) {
                                    ord.touch_filled = true;
                                    ord.touch_fill_ts = current_ts_ns;
                                    ord.touch_fill_mid = fill_mid;
                                    pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1, true, fill_mid});
                                    pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5, true, fill_mid});
                                    pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 30'000'000'000ULL, 30, true, fill_mid});
                                }
                                if (!ord.l3_full_filled) {
                                    uint32_t excess = 0;
                                    if (ref <= ord.arrival_ref_watermark) {
                                        // Real order was ahead of us in FIFO queue
                                        if (shares <= ord.queue_ahead) {
                                            ord.queue_ahead -= shares;
                                        } else {
                                            excess = shares - ord.queue_ahead;
                                            ord.queue_ahead = 0;
                                        }
                                    } else {
                                        // Real order behind us was executed; implies all volume ahead was exhausted
                                        ord.queue_ahead = 0;
                                        excess = shares;
                                    }

                                    if (excess > 0) {
                                        uint32_t needed = ord.target_shares - ord.executed_shares;
                                        uint32_t fill_qty = std::min(excess, needed);
                                        ord.executed_shares += fill_qty;

                                        if (!ord.l3_first_filled && ord.executed_shares >= 1) {
                                            ord.l3_first_filled = true;
                                            ord.l3_first_fill_ts = current_ts_ns;
                                            ord.l3_first_fill_mid = fill_mid;
                                            pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1, false, fill_mid});
                                            pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5, false, fill_mid});
                                            pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 30'000'000'000ULL, 30, false, fill_mid});
                                        }
                                        if (ord.executed_shares >= ord.target_shares) {
                                            ord.l3_full_filled = true;
                                            ord.l3_filled = true;
                                            ord.l3_fill_ts = current_ts_ns;
                                            ord.l3_fill_mid = fill_mid;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // If cancel, delete, or replace: adjust queue ahead for orders placed before ours
                else if ((msg_type == 'X' || msg_type == 'D' || msg_type == 'U') && pos.exists && pos.side == Side::Buy) {
                    uint32_t canceled = (msg_type == 'X') ? shares : pos.order_shares;
                    for (size_t ord_idx : active_resting_indices) {
                        SimOrder& ord = completed_orders[ord_idx];
                        if (ord.stock_locate == locate && ord.price == pos.price && current_ts_ns <= ord.cancel_ts_ns) {
                            if (ref <= ord.arrival_ref_watermark) {
                                ord.queue_ahead = (ord.queue_ahead > canceled) ? (ord.queue_ahead - canceled) : 0;
                            }
                        }
                    }
                }

                // Apply mutation to pure OrderBook
                switch (msg_type) {
                    case 'A':
                    case 'F':
                        book.add_order(ref, side, shares, price);
                        break;
                    case 'E':
                    case 'C':
                        book.execute_order(ref, shares);
                        break;
                    case 'X':
                        book.cancel_order(ref, shares);
                        break;
                    case 'D':
                        book.delete_order(ref);
                        break;
                    case 'U':
                        book.replace_order(ref, new_ref, shares, price);
                        break;
                    default:
                        break;
                }

                // Update valid mid cache
                auto bb = book.best_bid();
                auto ba = book.best_ask();
                if (bb && ba && ba->first > bb->first) {
                    last_valid_mid[locate] = (static_cast<double>(bb->first) + static_cast<double>(ba->first)) / 2.0;
                }

                // Submit simulated orders during regular hours strictly based on pure book state
                if (current_ts_ns >= MARKET_OPEN_NS && current_ts_ns < MARKET_CLOSE_NS) {
                    if (current_ts_ns - last_order_submit_ts >= config_.sampling_interval_ns) {
                        if (bb.has_value() && ba.has_value() && ba->first > bb->first) {
                            double v_bid = static_cast<double>(bb->second);
                            double v_ask = static_cast<double>(ba->second);
                            double obi = (v_bid - v_ask) / (v_bid + v_ask);

                            if (obi > config_.obi_threshold) {
                                SimOrder new_ord{};
                                new_ord.sim_id = ++next_sim_id;
                                new_ord.stock_locate = locate;
                                new_ord.side = Side::Buy;
                                new_ord.price = bb->first;
                                new_ord.target_shares = config_.order_shares;
                                new_ord.submit_ts_ns = current_ts_ns;
                                new_ord.arrival_ts_ns = current_ts_ns + config_.delta_t_ns;
                                new_ord.cancel_ts_ns = new_ord.arrival_ts_ns + config_.ttl_ns;
                                new_ord.time_block_id = static_cast<int>((current_ts_ns - MARKET_OPEN_NS) / (300ULL * 1'000'000'000ULL)); // 5-minute block

                                in_flight_orders.push_back(new_ord);
                                last_order_submit_ts = current_ts_ns;

                                // Drain immediate arrivals (e.g. delta_t == 0)
                                drain_arrivals();
                            }
                        }
                    }
                }
            }
        };

        // Streaming replay loop
        while (true) {
            int bytes_read = gzread(gz, buffer.data() + leftover, static_cast<unsigned int>(BUF_SIZE - leftover));
            if (bytes_read <= 0) break;

            size_t total_bytes = leftover + static_cast<size_t>(bytes_read);
            size_t offset = 0;

            while (offset + 2 <= total_bytes) {
                uint16_t msg_len = read_u16_be(buffer.data() + offset);
                if (offset + 2 + msg_len > total_bytes) break;

                const uint8_t* p = buffer.data() + offset + 2;
                char msg_type = static_cast<char>(p[0]);
                uint16_t locate = read_u16_be(p + 1);
                uint64_t ts = read_u48_be(p + 5);

                if (msg_type == 'R') {
                    std::string sym(reinterpret_cast<const char*>(p + 11), 8);
                    sym.erase(sym.find_last_not_of(" ") + 1);
                    locate_to_sym[locate] = sym;
                    OrderBookConfig cfg{
                        .order_pool_capacity = 131072,
                        .level_pool_capacity = 16384,
                        .order_map_capacity = 262144
                    };
                    books.emplace(locate, OrderBook(sym, locate, cfg));
                } else if (msg_type == 'A' || msg_type == 'F') {
                    uint64_t ref = read_u64_be(p + 11);
                    Side side = (p[19] == 'B') ? Side::Buy : Side::Sell;
                    uint32_t shares = read_u32_be(p + 20);
                    uint32_t price = read_u32_be(p + 32);
                    process_event('A', locate, ts, ref, shares, price, side, 0);
                } else if (msg_type == 'E') {
                    uint64_t ref = read_u64_be(p + 11);
                    uint32_t shares = read_u32_be(p + 19);
                    process_event('E', locate, ts, ref, shares, 0, Side::Buy, 0);
                } else if (msg_type == 'C') {
                    uint64_t ref = read_u64_be(p + 11);
                    uint32_t shares = read_u32_be(p + 19);
                    process_event('C', locate, ts, ref, shares, 0, Side::Buy, 0);
                } else if (msg_type == 'X') {
                    uint64_t ref = read_u64_be(p + 11);
                    uint32_t shares = read_u32_be(p + 19);
                    process_event('X', locate, ts, ref, shares, 0, Side::Buy, 0);
                } else if (msg_type == 'D') {
                    uint64_t ref = read_u64_be(p + 11);
                    process_event('D', locate, ts, ref, 0, 0, Side::Buy, 0);
                } else if (msg_type == 'U') {
                    uint64_t orig_ref = read_u64_be(p + 11);
                    uint64_t new_ref = read_u64_be(p + 19);
                    uint32_t shares = read_u32_be(p + 27);
                    uint32_t price = read_u32_be(p + 31);
                    process_event('U', locate, ts, orig_ref, shares, price, Side::Buy, new_ref);
                }

                offset += 2 + msg_len;
            }

            leftover = total_bytes - offset;
            if (leftover > 0) {
                std::memmove(buffer.data(), buffer.data() + offset, leftover);
            }
        }

        gzclose(gz);

        // Resolve any remaining pending markouts with last valid mid
        while (!pending_markouts.empty()) {
            PendingMarkout pm = pending_markouts.top();
            pending_markouts.pop();
            double mid = get_current_mid(pm.stock_locate);
            if (mid > 0.0 && pm.fill_price > 0) {
                double bps = ((mid - static_cast<double>(pm.fill_price)) / static_cast<double>(pm.fill_price)) * 10'000.0;
                auto& ord = completed_orders[pm.order_idx];
                if (pm.is_touch_model) {
                    if (pm.horizon_sec == 1 && !ord.touch_m1_done) { ord.touch_markout_1s_bps = bps; ord.touch_m1_done = true; }
                    if (pm.horizon_sec == 5 && !ord.touch_m5_done) { ord.touch_markout_5s_bps = bps; ord.touch_m5_done = true; }
                    if (pm.horizon_sec == 30 && !ord.touch_m30_done) { ord.touch_markout_30s_bps = bps; ord.touch_m30_done = true; }
                } else {
                    if (pm.horizon_sec == 1 && !ord.l3_m1_done) { ord.l3_markout_1s_bps = bps; ord.l3_m1_done = true; }
                    if (pm.horizon_sec == 5 && !ord.l3_m5_done) { ord.l3_markout_5s_bps = bps; ord.l3_m5_done = true; }
                    if (pm.horizon_sec == 30 && !ord.l3_m30_done) { ord.l3_markout_30s_bps = bps; ord.l3_m30_done = true; }
                }
            }
        }

        return completed_orders;
    }

private:
    std::string filepath_;
    SimulationConfig config_;
};

// Computes 5-minute time-block bootstrap confidence intervals for Stage 4 metrics
inline BootstrapCI compute_bootstrap_ci(const std::vector<double>& samples, double point_estimate) {
    if (samples.empty()) return {point_estimate, point_estimate, point_estimate};
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    size_t lo_idx = static_cast<size_t>(n * 0.025);
    size_t hi_idx = static_cast<size_t>(n * 0.975);
    if (hi_idx >= n) hi_idx = n - 1;
    return {point_estimate, sorted[lo_idx], sorted[hi_idx]};
}

// Aggregates Model metrics and calculates Block Bootstrap CIs
inline FillModelSummary evaluate_model_performance(
    const std::vector<SimOrder>& orders,
    bool is_touch_model,
    const std::string& model_name,
    uint64_t delta_t_ns,
    size_t reference_l3_full_fills = 0)
{
    FillModelSummary summary{};
    summary.model_name = model_name;
    summary.delta_t_ns = delta_t_ns;
    summary.total_orders = orders.size();

    double sum_half_spread = 0.0;
    double sum_m1 = 0.0, sum_m5 = 0.0, sum_m30 = 0.0;
    double sum_pnl_5s = 0.0, sum_pnl_30s = 0.0;
    size_t valid_fills = 0;

    for (const auto& ord : orders) {
        bool f_first = is_touch_model ? ord.touch_filled : ord.l3_first_filled;
        bool f_full = is_touch_model ? ord.touch_filled : ord.l3_full_filled;

        if (f_first) summary.total_first_fills++;
        if (f_full) summary.total_full_fills++;

        // Headline fill evaluation (full fill for L3, touch for Touch model)
        bool filled = f_full;
        if (filled) {
            summary.total_fills++;
            double fill_mid = is_touch_model ? ord.touch_fill_mid : ord.l3_fill_mid;
            if (fill_mid > 0.0 && ord.price > 0) {
                double half_sp = ((fill_mid - static_cast<double>(ord.price)) / static_cast<double>(ord.price)) * 10'000.0;
                sum_half_spread += half_sp;
            }

            double m1 = is_touch_model ? ord.touch_markout_1s_bps : ord.l3_markout_1s_bps;
            double m5 = is_touch_model ? ord.touch_markout_5s_bps : ord.l3_markout_5s_bps;
            double m30 = is_touch_model ? ord.touch_markout_30s_bps : ord.l3_markout_30s_bps;

            sum_m1 += m1;
            sum_m5 += m5;
            sum_m30 += m30;

            // Mark-to-mid markout PnL in USD
            double scale = static_cast<double>(ord.target_shares) / 100.0;
            double pnl_5s = ((m5 * static_cast<double>(ord.price)) / 1'000'000.0) * scale;
            double pnl_30s = ((m30 * static_cast<double>(ord.price)) / 1'000'000.0) * scale;
            sum_pnl_5s += pnl_5s;
            sum_pnl_30s += pnl_30s;
            valid_fills++;
        }
    }

    if (valid_fills > 0) {
        summary.avg_half_spread_captured_bps = sum_half_spread / static_cast<double>(valid_fills);
        summary.total_pnl_5s_usd = sum_pnl_5s;
        summary.total_pnl_30s_usd = sum_pnl_30s;
        summary.pnl_per_order_5s_usd = summary.total_orders > 0 ? (sum_pnl_5s / static_cast<double>(summary.total_orders)) : 0.0;
        summary.pnl_per_order_30s_usd = summary.total_orders > 0 ? (sum_pnl_30s / static_cast<double>(summary.total_orders)) : 0.0;
        double point_m30 = sum_m30 / static_cast<double>(valid_fills);
        summary.mid_drift_30s_bps = point_m30 - summary.avg_half_spread_captured_bps;
    }

    double point_first_rate = summary.total_orders > 0 ? (100.0 * summary.total_first_fills / summary.total_orders) : 0.0;
    double point_full_rate = summary.total_orders > 0 ? (100.0 * summary.total_full_fills / summary.total_orders) : 0.0;
    double point_overstate = reference_l3_full_fills > 0 ? (static_cast<double>(summary.total_full_fills) / reference_l3_full_fills) : 1.0;

    // 5-minute time-block bootstrapping (78 blocks)
    constexpr int NUM_BLOCKS = 78;
    constexpr int BOOTSTRAP_ITERS = 1000;
    std::vector<std::vector<size_t>> block_order_indices(NUM_BLOCKS);
    for (size_t i = 0; i < orders.size(); ++i) {
        int bid = orders[i].time_block_id;
        if (bid >= 0 && bid < NUM_BLOCKS) {
            block_order_indices[bid].push_back(i);
        }
    }

    std::mt19937_64 rng(42 + delta_t_ns + (is_touch_model ? 1000 : 0));
    std::uniform_int_distribution<int> block_dist(0, NUM_BLOCKS - 1);

    std::vector<double> bs_first_fill_rates;
    std::vector<double> bs_full_fill_rates;
    std::vector<double> bs_overstate;
    std::vector<double> bs_lead_time;
    std::vector<double> bs_m1, bs_m5, bs_m30;
    std::vector<double> bs_pnl_5s;
    std::vector<double> bs_pnl_30s;
    std::vector<double> bs_pnl_ratios;

    bs_first_fill_rates.reserve(BOOTSTRAP_ITERS);
    bs_full_fill_rates.reserve(BOOTSTRAP_ITERS);
    bs_overstate.reserve(BOOTSTRAP_ITERS);
    bs_lead_time.reserve(BOOTSTRAP_ITERS);
    bs_m1.reserve(BOOTSTRAP_ITERS);
    bs_m5.reserve(BOOTSTRAP_ITERS);
    bs_m30.reserve(BOOTSTRAP_ITERS);
    bs_pnl_5s.reserve(BOOTSTRAP_ITERS);
    bs_pnl_30s.reserve(BOOTSTRAP_ITERS);
    bs_pnl_ratios.reserve(BOOTSTRAP_ITERS);

    for (int iter = 0; iter < BOOTSTRAP_ITERS; ++iter) {
        size_t b_tot = 0;
        size_t b_first_fills = 0;
        size_t b_full_fills = 0;
        size_t b_ref_full_fills = 0;
        double b_sum_lead = 0.0;
        size_t b_lead_count = 0;
        double b_sum_m1 = 0.0, b_sum_m5 = 0.0, b_sum_m30 = 0.0;
        double b_sum_pnl5 = 0.0, b_sum_pnl30 = 0.0;
        double b_l3_pnl5 = 0.0;

        for (int k = 0; k < NUM_BLOCKS; ++k) {
            int sampled_bid = block_dist(rng);
            for (size_t idx : block_order_indices[sampled_bid]) {
                const auto& ord = orders[idx];
                b_tot++;
                double scale = static_cast<double>(ord.target_shares) / 100.0;
                bool f_first = is_touch_model ? ord.touch_filled : ord.l3_first_filled;
                bool f_full = is_touch_model ? ord.touch_filled : ord.l3_full_filled;

                if (f_first) b_first_fills++;
                if (f_full) {
                    b_full_fills++;
                    double m1 = is_touch_model ? ord.touch_markout_1s_bps : ord.l3_markout_1s_bps;
                    double m5 = is_touch_model ? ord.touch_markout_5s_bps : ord.l3_markout_5s_bps;
                    double m30 = is_touch_model ? ord.touch_markout_30s_bps : ord.l3_markout_30s_bps;
                    b_sum_m1 += m1;
                    b_sum_m5 += m5;
                    b_sum_m30 += m30;
                    b_sum_pnl5 += ((m5 * static_cast<double>(ord.price)) / 1'000'000.0) * scale;
                    b_sum_pnl30 += ((m30 * static_cast<double>(ord.price)) / 1'000'000.0) * scale;
                }
                if (ord.l3_full_filled) {
                    b_ref_full_fills++;
                    b_l3_pnl5 += ((ord.l3_markout_5s_bps * static_cast<double>(ord.price)) / 1'000'000.0) * scale;
                }
                if (ord.touch_filled && ord.l3_full_filled) {
                    double lead_ms = (static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.touch_fill_ts)) / 1e6;
                    b_sum_lead += lead_ms;
                    b_lead_count++;
                }
            }
        }

        if (b_tot > 0) {
            bs_first_fill_rates.push_back(100.0 * b_first_fills / b_tot);
            bs_full_fill_rates.push_back(100.0 * b_full_fills / b_tot);
            if (b_ref_full_fills > 0 && is_touch_model) {
                bs_overstate.push_back(static_cast<double>(b_full_fills) / b_ref_full_fills);
            }
            if (b_lead_count > 0 && is_touch_model) {
                bs_lead_time.push_back(b_sum_lead / b_lead_count);
            }
            if (b_full_fills > 0) {
                bs_m1.push_back(b_sum_m1 / b_full_fills);
                bs_m5.push_back(b_sum_m5 / b_full_fills);
                bs_m30.push_back(b_sum_m30 / b_full_fills);
                bs_pnl_5s.push_back(b_sum_pnl5);
                bs_pnl_30s.push_back(b_sum_pnl30);
            }
            if (is_touch_model && b_l3_pnl5 > 0.0 && b_sum_pnl5 > 0.0) {
                bs_pnl_ratios.push_back(b_sum_pnl5 / b_l3_pnl5);
            }
        }
    }

    double point_m1 = valid_fills > 0 ? (sum_m1 / valid_fills) : 0.0;
    double point_m5 = valid_fills > 0 ? (sum_m5 / valid_fills) : 0.0;
    double point_m30 = valid_fills > 0 ? (sum_m30 / valid_fills) : 0.0;

    double point_lead = 0.0;
    size_t lead_n = 0;
    double lead_sum = 0.0;
    for (const auto& ord : orders) {
        if (ord.touch_filled && ord.l3_full_filled) {
            lead_n++;
            lead_sum += (static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.touch_fill_ts)) / 1e6;
        }
    }
    if (lead_n > 0) point_lead = lead_sum / lead_n;

    summary.first_fill_rate_ci = compute_bootstrap_ci(bs_first_fill_rates, point_first_rate);
    summary.full_fill_rate_ci = compute_bootstrap_ci(bs_full_fill_rates, point_full_rate);
    summary.fill_rate_ci = summary.full_fill_rate_ci;
    summary.overstatement_ratio_ci = compute_bootstrap_ci(bs_overstate, point_overstate);
    summary.mean_lead_time_ms_ci = compute_bootstrap_ci(bs_lead_time, point_lead);
    summary.markout_1s_ci = compute_bootstrap_ci(bs_m1, point_m1);
    summary.markout_5s_ci = compute_bootstrap_ci(bs_m5, point_m5);
    summary.markout_30s_ci = compute_bootstrap_ci(bs_m30, point_m30);
    summary.pnl_5s_usd_ci = compute_bootstrap_ci(bs_pnl_5s, summary.total_pnl_5s_usd);
    summary.pnl_30s_usd_ci = compute_bootstrap_ci(bs_pnl_30s, summary.total_pnl_30s_usd);

    if (is_touch_model && !bs_pnl_ratios.empty()) {
        double pt_ratio = bs_pnl_ratios.front();
        summary.pnl_ratio_5s_ci = compute_bootstrap_ci(bs_pnl_ratios, pt_ratio);
    }

    return summary;
}

} // namespace l3
