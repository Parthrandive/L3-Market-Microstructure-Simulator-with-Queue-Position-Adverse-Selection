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
    uint32_t shares{100};
    uint64_t submit_ts_ns{0};     // Strategy signal timestamp
    uint64_t arrival_ts_ns{0};    // Arrival at matching engine (submit_ts + delta_t)
    uint64_t cancel_ts_ns{0};     // TTL expiration timestamp (arrival_ts + ttl)
    int time_block_id{0};         // 5-minute block id (0..77 for regular trading hours)

    // Initial state upon book entry
    uint32_t initial_queue_ahead{0};
    double initial_spread_bps{0.0};
    double entry_mid{0.0};

    // 1. Model 1: Naive Touch Model
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
    bool entered_book{false};
    bool l3_filled{false};
    bool l3_canceled{false};
    uint64_t l3_fill_ts{0};
    double l3_fill_mid{0.0};
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

struct FillModelSummary {
    std::string model_name;
    uint64_t delta_t_ns{0};
    size_t total_orders{0};
    size_t total_fills{0};

    BootstrapCI fill_rate_ci{};
    BootstrapCI overstatement_ratio_ci{};
    BootstrapCI mean_lead_time_ms_ci{};

    double avg_half_spread_captured_bps{0.0};
    BootstrapCI markout_1s_ci{};
    BootstrapCI markout_5s_ci{};
    BootstrapCI markout_30s_ci{};
    double mid_drift_30s_bps{0.0}; // markout_30s - half_spread

    // Realized Strategy PnL (for 100 shares per fill)
    double total_pnl_5s_usd{0.0};
    double pnl_per_order_5s_usd{0.0};
    double total_pnl_30s_usd{0.0};
    double pnl_per_order_30s_usd{0.0};
    BootstrapCI pnl_30s_usd_ci{};
};

struct SimulationConfig {
    uint64_t delta_t_ns{0};                  // Wire transit latency
    uint64_t ttl_ns{5'000'000'000ULL};       // 5.0 seconds TTL
    double obi_threshold{0.0};               // Order Book Imbalance filter
    uint64_t sampling_interval_ns{250'000'000ULL}; // 250 ms sampling rate
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
        std::vector<size_t> resting_sim_indices;

        std::priority_queue<PendingMarkout, std::vector<PendingMarkout>, std::greater<PendingMarkout>> pending_markouts;

        constexpr size_t BUF_SIZE = 65536;
        std::vector<uint8_t> buffer(BUF_SIZE);
        size_t leftover = 0;
        uint64_t current_ts_ns = 0;
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

        auto process_event = [&](char msg_type, uint16_t locate, uint64_t ts,
                                 uint64_t ref, uint32_t shares, uint32_t price, Side side, uint64_t new_ref) {
            current_ts_ns = ts;

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
            while (!in_flight_orders.empty() && in_flight_orders.front().arrival_ts_ns <= current_ts_ns) {
                SimOrder ord = in_flight_orders.front();
                in_flight_orders.pop_front();

                auto book_it = books.find(ord.stock_locate);
                if (book_it != books.end()) {
                    auto bbo = book_it->second.best_bid();
                    auto ask = book_it->second.best_ask();
                    ord.entered_book = true;
                    uint32_t depth_ahead = 0;
                    if (bbo && bbo->first == ord.price) {
                        depth_ahead = bbo->second;
                    }
                    ord.initial_queue_ahead = depth_ahead;
                    if (bbo && ask && ask->first > bbo->first) {
                        ord.entry_mid = (static_cast<double>(bbo->first) + static_cast<double>(ask->first)) / 2.0;
                        ord.initial_spread_bps = ((static_cast<double>(ask->first) - static_cast<double>(bbo->first)) / ord.entry_mid) * 10'000.0;
                    }

                    // Add order to L3 engine at the tail of the level
                    book_it->second.add_order(ord.sim_id, ord.side, ord.shares, ord.price);

                    size_t ord_idx = completed_orders.size();
                    completed_orders.push_back(ord);
                    resting_sim_indices.push_back(ord_idx);
                }
            }

            // 3. Check resting TTL expiration
            for (auto it = resting_sim_indices.begin(); it != resting_sim_indices.end();) {
                SimOrder& ord = completed_orders[*it];
                if (!ord.l3_filled && !ord.l3_canceled && current_ts_ns >= ord.cancel_ts_ns) {
                    auto b_it = books.find(ord.stock_locate);
                    if (b_it != books.end()) {
                        b_it->second.cancel_order(ord.sim_id, ord.shares);
                    }
                    ord.l3_canceled = true;
                    it = resting_sim_indices.erase(it);
                } else if (ord.l3_filled || ord.l3_canceled) {
                    it = resting_sim_indices.erase(it);
                } else {
                    ++it;
                }
            }

            // 4. Process market event
            auto book_it = books.find(locate);
            if (book_it != books.end()) {
                OrderBook& book = book_it->second;

                bool is_target_level = false;
                if (msg_type == 'E' || msg_type == 'C' || msg_type == 'X' || msg_type == 'D') {
                    auto pos = book.get_queue_position(ref);
                    if (pos.exists) {
                        price = pos.price;
                        side = pos.side;
                        is_target_level = true;
                    }
                }

                // If execution: check simulated orders
                if ((msg_type == 'E' || msg_type == 'C') && is_target_level) {
                    double fill_mid = get_current_mid(locate);

                    for (size_t ord_idx : resting_sim_indices) {
                        SimOrder& ord = completed_orders[ord_idx];
                        if (ord.stock_locate == locate && ord.price == price && ord.side == side) {
                            // Model 1: Naive Touch Fill trigger (first trade touches limit price)
                            if (!ord.touch_filled && current_ts_ns <= ord.cancel_ts_ns) {
                                ord.touch_filled = true;
                                ord.touch_fill_ts = current_ts_ns;
                                ord.touch_fill_mid = fill_mid;
                                pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1, true, fill_mid});
                                pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5, true, fill_mid});
                                pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 30'000'000'000ULL, 30, true, fill_mid});
                            }

                            // Model 2 / Model 3: Queue-Aware L3 priority fill trigger
                            if (!ord.l3_filled && current_ts_ns <= ord.cancel_ts_ns) {
                                auto qpos_before = book.get_queue_position(ord.sim_id);
                                if (qpos_before.exists) {
                                    bool ahead = book.is_order_ahead(ref, ord.sim_id);
                                    bool execute_sim = false;

                                    if (qpos_before.shares_ahead == 0) {
                                        execute_sim = true;
                                    } else if (!ahead && ref != ord.sim_id) {
                                        // A subsequent order was executed, so our priority was cleared
                                        execute_sim = true;
                                    } else if (ahead && shares > qpos_before.shares_ahead) {
                                        execute_sim = true;
                                    }

                                    if (execute_sim) {
                                        ord.l3_filled = true;
                                        ord.l3_fill_ts = current_ts_ns;
                                        ord.l3_fill_mid = fill_mid;

                                        pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1, false, fill_mid});
                                        pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5, false, fill_mid});
                                        pending_markouts.push({ord_idx, locate, ord.price, current_ts_ns + 30'000'000'000ULL, 30, false, fill_mid});

                                        // Cleanly remove from matching engine to prevent ghost shares
                                        book.execute_order(ord.sim_id, ord.shares);
                                    }
                                }
                            }
                        }
                    }
                }

                // Apply mutation to book
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

                // Submit simulated orders during regular hours
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
                                new_ord.shares = 100;
                                new_ord.submit_ts_ns = current_ts_ns;
                                new_ord.arrival_ts_ns = current_ts_ns + config_.delta_t_ns;
                                new_ord.cancel_ts_ns = new_ord.arrival_ts_ns + config_.ttl_ns;
                                new_ord.time_block_id = static_cast<int>((current_ts_ns - MARKET_OPEN_NS) / (300ULL * 1'000'000'000ULL)); // 5-minute block

                                in_flight_orders.push_back(new_ord);
                                last_order_submit_ts = current_ts_ns;
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
    size_t reference_l3_fills = 0)
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
        bool filled = is_touch_model ? ord.touch_filled : ord.l3_filled;
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

            // PnL in USD for 100 shares: 100 * (mid - price) / 10000 = (mid - price) / 100
            // Since markout_bps = ((mid - price) / price) * 10000:
            // (mid - price) = (markout_bps * price) / 10000
            // PnL ($) = 100 * (markout_bps * price / 10000) / 10000 = (markout_bps * price) / 1'000'000
            double pnl_5s = (m5 * static_cast<double>(ord.price)) / 1'000'000.0;
            double pnl_30s = (m30 * static_cast<double>(ord.price)) / 1'000'000.0;
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

    double point_fill_rate = summary.total_orders > 0 ? (100.0 * summary.total_fills / summary.total_orders) : 0.0;
    double point_overstate = reference_l3_fills > 0 ? (static_cast<double>(summary.total_fills) / reference_l3_fills) : 1.0;

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

    std::vector<double> bs_fill_rates;
    std::vector<double> bs_overstate;
    std::vector<double> bs_lead_time;
    std::vector<double> bs_m1, bs_m5, bs_m30;
    std::vector<double> bs_pnl_30s;

    bs_fill_rates.reserve(BOOTSTRAP_ITERS);
    bs_overstate.reserve(BOOTSTRAP_ITERS);
    bs_lead_time.reserve(BOOTSTRAP_ITERS);
    bs_m1.reserve(BOOTSTRAP_ITERS);
    bs_m5.reserve(BOOTSTRAP_ITERS);
    bs_m30.reserve(BOOTSTRAP_ITERS);
    bs_pnl_30s.reserve(BOOTSTRAP_ITERS);

    for (int iter = 0; iter < BOOTSTRAP_ITERS; ++iter) {
        size_t b_tot = 0;
        size_t b_fills = 0;
        size_t b_ref_fills = 0;
        double b_sum_lead = 0.0;
        size_t b_lead_count = 0;
        double b_sum_m1 = 0.0, b_sum_m5 = 0.0, b_sum_m30 = 0.0;
        double b_sum_pnl30 = 0.0;

        for (int k = 0; k < NUM_BLOCKS; ++k) {
            int sampled_bid = block_dist(rng);
            for (size_t idx : block_order_indices[sampled_bid]) {
                const auto& ord = orders[idx];
                b_tot++;
                bool f = is_touch_model ? ord.touch_filled : ord.l3_filled;
                if (f) {
                    b_fills++;
                    double m1 = is_touch_model ? ord.touch_markout_1s_bps : ord.l3_markout_1s_bps;
                    double m5 = is_touch_model ? ord.touch_markout_5s_bps : ord.l3_markout_5s_bps;
                    double m30 = is_touch_model ? ord.touch_markout_30s_bps : ord.l3_markout_30s_bps;
                    b_sum_m1 += m1;
                    b_sum_m5 += m5;
                    b_sum_m30 += m30;
                    b_sum_pnl30 += (m30 * static_cast<double>(ord.price)) / 1'000'000.0;
                }
                if (ord.l3_filled) {
                    b_ref_fills++;
                }
                if (ord.touch_filled && ord.l3_filled) {
                    double lead_ms = (static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.touch_fill_ts)) / 1e6;
                    b_sum_lead += lead_ms;
                    b_lead_count++;
                }
            }
        }

        if (b_tot > 0) {
            bs_fill_rates.push_back(100.0 * b_fills / b_tot);
            if (b_ref_fills > 0 && is_touch_model) {
                bs_overstate.push_back(static_cast<double>(b_fills) / b_ref_fills);
            }
            if (b_lead_count > 0 && is_touch_model) {
                bs_lead_time.push_back(b_sum_lead / b_lead_count);
            }
            if (b_fills > 0) {
                bs_m1.push_back(b_sum_m1 / b_fills);
                bs_m5.push_back(b_sum_m5 / b_fills);
                bs_m30.push_back(b_sum_m30 / b_fills);
                bs_pnl_30s.push_back(b_sum_pnl30);
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
        if (ord.touch_filled && ord.l3_filled) {
            lead_n++;
            lead_sum += (static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.touch_fill_ts)) / 1e6;
        }
    }
    if (lead_n > 0) point_lead = lead_sum / lead_n;

    summary.fill_rate_ci = compute_bootstrap_ci(bs_fill_rates, point_fill_rate);
    summary.overstatement_ratio_ci = compute_bootstrap_ci(bs_overstate, point_overstate);
    summary.mean_lead_time_ms_ci = compute_bootstrap_ci(bs_lead_time, point_lead);
    summary.markout_1s_ci = compute_bootstrap_ci(bs_m1, point_m1);
    summary.markout_5s_ci = compute_bootstrap_ci(bs_m5, point_m5);
    summary.markout_30s_ci = compute_bootstrap_ci(bs_m30, point_m30);
    summary.pnl_30s_usd_ci = compute_bootstrap_ci(bs_pnl_30s, summary.total_pnl_30s_usd);

    return summary;
}

} // namespace l3
