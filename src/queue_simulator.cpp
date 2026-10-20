#include <iostream>
#include <iomanip>
#include <vector>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <string>
#include <cmath>
#include <random>
#include <algorithm>
#include "orderbook/itch_parser.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/queue_tracker.hpp"

using namespace l3;

// Strategy order definition
struct SimOrder {
    uint64_t sim_id{0};
    uint16_t stock_locate{0};
    Side side{Side::Buy};
    uint32_t price{0};
    uint32_t shares{100};
    uint64_t submit_ts_ns{0};
    uint64_t arrival_ts_ns{0};
    uint64_t cancel_ts_ns{0};
    int time_block_id{0}; // 5-minute block for bootstrapping

    bool entered_book{false};
    bool l3_filled{false};
    bool l3_canceled{false};
    uint64_t l3_fill_ts{0};

    // L2 Models
    L2QueueState pessimistic{};
    L2QueueState probabilistic{};
    bool naive_touch_filled{false};
    uint64_t naive_touch_fill_ts{0};

    uint32_t initial_queue_ahead{0};
    uint64_t sample_count{0};
    double sum_err_pess{0.0};
    double sum_abs_err_pess{0.0};
    double sum_err_prob{0.0};
    double sum_abs_err_prob{0.0};
    double sum_rel_err_pess{0.0};
    double sum_rel_err_prob{0.0};

    // Markout tracking
    bool markout_1s_done{false};
    double markout_1s_bps{0.0};
    bool markout_5s_done{false};
    double markout_5s_bps{0.0};
};

struct PendingMarkout {
    size_t order_idx{0};
    uint16_t stock_locate{0};
    uint32_t fill_price{0};
    uint64_t target_ts_ns{0};
    int horizon_sec{0}; // 1 or 5
};

struct BootstrapCI {
    double point_est{0.0};
    double ci_lo{0.0};
    double ci_hi{0.0};
};

struct QueueDepthBucketStats {
    std::string label;
    size_t sample_count{0};
    double avg_depth_ahead{0.0};
    double pess_bias_sh{0.0};
    double pess_mae_sh{0.0};
    double pess_rel_err_pct{0.0};
    double prob_bias_sh{0.0};
    double prob_mae_sh{0.0};
    double prob_rel_err_pct{0.0};
};

struct LatencyRunResult {
    uint64_t delta_t_ns{0};
    size_t total_orders{0};
    size_t l3_fills{0};
    size_t touch_fills{0};
    size_t pess_fills{0};
    size_t prob_fills{0};
    size_t touch_false_positives{0};

    BootstrapCI l3_fill_rate_ci{};
    BootstrapCI touch_fill_rate_ci{};
    BootstrapCI overstatement_ratio_ci{};
    BootstrapCI mean_lead_time_ms_ci{};

    double mean_markout_1s_bps{0.0};
    double mean_markout_5s_bps{0.0};
    size_t markout_count{0};

    std::vector<QueueDepthBucketStats> depth_buckets;
};

class Stage3SimulationEngine {
public:
    Stage3SimulationEngine(std::string path, uint64_t delta_t_ns, uint64_t ttl_ns = 5'000'000'000ULL, double obi_threshold = 0.0)
        : filepath_(std::move(path)), delta_t_ns_(delta_t_ns), ttl_ns_(ttl_ns), obi_threshold_(obi_threshold) {}

    LatencyRunResult run() {
        gzFile gz = gzopen(filepath_.c_str(), "rb");
        if (!gz) {
            std::cerr << "Failed to open " << filepath_ << "\n";
            return {};
        }

        std::unordered_map<uint16_t, std::string> locate_to_sym;
        std::unordered_map<uint16_t, OrderBook> books;
        std::vector<SimOrder> completed_orders;
        completed_orders.reserve(100000);
        std::deque<SimOrder> in_flight_orders;
        std::vector<size_t> resting_sim_indices;
        std::deque<PendingMarkout> pending_markouts;

        constexpr size_t BUF_SIZE = 65536;
        std::vector<uint8_t> buffer(BUF_SIZE);
        size_t leftover = 0;
        uint64_t current_ts_ns = 0;
        uint64_t next_sim_id = 900'000'000'000ULL;
        uint64_t last_order_submit_ts = 0;

        // Strictly regular trading hours: 09:30:00 to 16:00:00 ET
        constexpr uint64_t MARKET_OPEN_NS = 9ULL * 3600ULL * 1'000'000'000ULL + 30ULL * 60ULL * 1'000'000'000ULL;
        constexpr uint64_t MARKET_CLOSE_NS = 16ULL * 3600ULL * 1'000'000'000ULL;
        constexpr uint64_t ORDER_INTERVAL_NS = 250'000'000ULL; // 250 ms sampling interval for deep sample size

        auto process_event = [&](char msg_type, uint16_t locate, uint64_t ts,
                                 uint64_t ref, uint32_t shares, uint32_t price, Side side, uint64_t new_ref) {
            current_ts_ns = ts;

            // 1. Process pending markout checks
            while (!pending_markouts.empty() && pending_markouts.front().target_ts_ns <= current_ts_ns) {
                auto pm = pending_markouts.front();
                pending_markouts.pop_front();
                auto b_it = books.find(pm.stock_locate);
                if (b_it != books.end()) {
                    auto bb = b_it->second.best_bid();
                    auto ba = b_it->second.best_ask();
                    if (bb && ba && ba->first > bb->first) {
                        double mid = (static_cast<double>(bb->first) + static_cast<double>(ba->first)) / 2.0;
                        double bps = ((mid - static_cast<double>(pm.fill_price)) / static_cast<double>(pm.fill_price)) * 10'000.0;
                        if (pm.horizon_sec == 1) {
                            completed_orders[pm.order_idx].markout_1s_bps = bps;
                            completed_orders[pm.order_idx].markout_1s_done = true;
                        } else if (pm.horizon_sec == 5) {
                            completed_orders[pm.order_idx].markout_5s_bps = bps;
                            completed_orders[pm.order_idx].markout_5s_done = true;
                        }
                    }
                }
            }

            // 2. In-flight order arrival after wire delay
            while (!in_flight_orders.empty() && in_flight_orders.front().arrival_ts_ns <= current_ts_ns) {
                SimOrder ord = in_flight_orders.front();
                in_flight_orders.pop_front();

                auto book_it = books.find(ord.stock_locate);
                if (book_it != books.end()) {
                    auto bbo = book_it->second.best_bid();
                    ord.entered_book = true;
                    uint32_t depth_ahead = 0;
                    if (bbo && bbo->first == ord.price) {
                        depth_ahead = bbo->second;
                    }
                    ord.initial_queue_ahead = depth_ahead;
                    ord.pessimistic.init(depth_ahead, ord.shares);
                    ord.probabilistic.init(depth_ahead, ord.shares);

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

                uint32_t depth_before = 0;
                bool is_target_level = false;
                if (msg_type == 'E' || msg_type == 'C' || msg_type == 'X' || msg_type == 'D') {
                    auto pos = book.get_queue_position(ref);
                    if (pos.exists) {
                        depth_before = pos.level_total_shares;
                        price = pos.price;
                        side = pos.side;
                        is_target_level = true;
                    }
                }

                // If execution: check resting simulated orders
                if ((msg_type == 'E' || msg_type == 'C') && is_target_level) {
                    for (size_t ord_idx : resting_sim_indices) {
                        SimOrder& ord = completed_orders[ord_idx];
                        if (ord.stock_locate == locate && ord.price == price && ord.side == side && !ord.l3_filled) {
                            if (!ord.naive_touch_filled) {
                                ord.naive_touch_filled = true;
                                ord.naive_touch_fill_ts = current_ts_ns;
                            }

                            auto qpos_before = book.get_queue_position(ord.sim_id);
                            if (qpos_before.exists) {
                                bool ahead = book.is_order_ahead(ref, ord.sim_id);
                                if (qpos_before.shares_ahead == 0) {
                                    ord.l3_filled = true;
                                    ord.l3_fill_ts = current_ts_ns;
                                } else if (!ahead && ref != ord.sim_id) {
                                    ord.l3_filled = true;
                                    ord.l3_fill_ts = current_ts_ns;
                                } else if (ahead && shares > qpos_before.shares_ahead) {
                                    ord.l3_filled = true;
                                    ord.l3_fill_ts = current_ts_ns;
                                }

                                if (ord.l3_filled) {
                                    pending_markouts.push_back({ord_idx, locate, ord.price, current_ts_ns + 1'000'000'000ULL, 1});
                                    pending_markouts.push_back({ord_idx, locate, ord.price, current_ts_ns + 5'000'000'000ULL, 5});
                                }
                            }

                            ord.pessimistic.on_execution(shares, current_ts_ns);
                            ord.probabilistic.on_execution(shares, current_ts_ns);
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

                // Post-mutation updates for cancels
                if ((msg_type == 'X' || msg_type == 'D') && is_target_level) {
                    for (size_t ord_idx : resting_sim_indices) {
                        SimOrder& ord = completed_orders[ord_idx];
                        if (ord.stock_locate == locate && ord.price == price && ord.side == side && !ord.l3_filled) {
                            auto qpos_after = book.get_queue_position(ord.sim_id);
                            uint32_t depth_after = qpos_after.exists ? qpos_after.level_total_shares : 0;
                            ord.pessimistic.on_cancel_pessimistic(depth_after);
                            ord.probabilistic.on_cancel_probabilistic(shares, depth_before, depth_after);
                        }
                    }
                }

                // Sample error stats for resting orders
                for (size_t ord_idx : resting_sim_indices) {
                    SimOrder& ord = completed_orders[ord_idx];
                    if (ord.stock_locate == locate && ord.price == price && ord.side == side && !ord.l3_filled) {
                        auto qpos = book.get_queue_position(ord.sim_id);
                        if (qpos.exists) {
                            double q_true = static_cast<double>(qpos.shares_ahead);
                            ord.sample_count++;

                            double err_pess = ord.pessimistic.queue_ahead - q_true;
                            ord.sum_err_pess += err_pess;
                            ord.sum_abs_err_pess += std::abs(err_pess);

                            double err_prob = ord.probabilistic.queue_ahead - q_true;
                            ord.sum_err_prob += err_prob;
                            ord.sum_abs_err_prob += std::abs(err_prob);
                        }
                    }
                }

                // Submit simulated orders during regular hours
                if (current_ts_ns >= MARKET_OPEN_NS && current_ts_ns < MARKET_CLOSE_NS) {
                    if (current_ts_ns - last_order_submit_ts >= ORDER_INTERVAL_NS) {
                        auto best_b = book.best_bid();
                        auto best_a = book.best_ask();
                        if (best_b.has_value() && best_a.has_value() && best_a->first > best_b->first) {
                            // Check OBI
                            double v_bid = static_cast<double>(best_b->second);
                            double v_ask = static_cast<double>(best_a->second);
                            double obi = (v_bid - v_ask) / (v_bid + v_ask);

                            // Strategy variant: join best bid when OBI > obi_threshold_
                            if (obi > obi_threshold_) {
                                SimOrder new_ord{};
                                new_ord.sim_id = ++next_sim_id;
                                new_ord.stock_locate = locate;
                                new_ord.side = Side::Buy;
                                new_ord.price = best_b->first;
                                new_ord.shares = 100;
                                new_ord.submit_ts_ns = current_ts_ns;
                                new_ord.arrival_ts_ns = current_ts_ns + delta_t_ns_;
                                new_ord.cancel_ts_ns = new_ord.arrival_ts_ns + ttl_ns_;
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

        // Aggregate results
        LatencyRunResult res{};
        res.delta_t_ns = delta_t_ns_;
        res.total_orders = completed_orders.size();

        double sum_markout_1s = 0.0;
        double sum_markout_5s = 0.0;

        for (const auto& ord : completed_orders) {
            if (ord.l3_filled) res.l3_fills++;
            if (ord.naive_touch_filled) {
                res.touch_fills++;
                if (!ord.l3_filled) res.touch_false_positives++;
            }
            if (ord.pessimistic.filled) res.pess_fills++;
            if (ord.probabilistic.filled) res.prob_fills++;

            if (ord.l3_filled && ord.markout_1s_done && ord.markout_5s_done) {
                sum_markout_1s += ord.markout_1s_bps;
                sum_markout_5s += ord.markout_5s_bps;
                res.markout_count++;
            }
        }

        if (res.markout_count > 0) {
            res.mean_markout_1s_bps = sum_markout_1s / static_cast<double>(res.markout_count);
            res.mean_markout_5s_bps = sum_markout_5s / static_cast<double>(res.markout_count);
        }

        // 5. Block bootstrap by 5-minute time blocks (78 blocks)
        constexpr int NUM_BLOCKS = 78;
        constexpr int BOOTSTRAP_ITERS = 1000;
        std::vector<std::vector<size_t>> block_order_indices(NUM_BLOCKS);
        for (size_t i = 0; i < completed_orders.size(); ++i) {
            int bid = completed_orders[i].time_block_id;
            if (bid >= 0 && bid < NUM_BLOCKS) {
                block_order_indices[bid].push_back(i);
            }
        }

        std::mt19937_64 rng(42);
        std::uniform_int_distribution<int> block_dist(0, NUM_BLOCKS - 1);

        std::vector<double> bs_l3_rate;
        std::vector<double> bs_touch_rate;
        std::vector<double> bs_overstate;
        std::vector<double> bs_lead_time;
        bs_l3_rate.reserve(BOOTSTRAP_ITERS);
        bs_touch_rate.reserve(BOOTSTRAP_ITERS);
        bs_overstate.reserve(BOOTSTRAP_ITERS);
        bs_lead_time.reserve(BOOTSTRAP_ITERS);

        for (int it = 0; it < BOOTSTRAP_ITERS; ++it) {
            size_t b_tot = 0;
            size_t b_l3 = 0;
            size_t b_touch = 0;
            double b_sum_lead_ms = 0.0;
            size_t b_lead_count = 0;

            for (int k = 0; k < NUM_BLOCKS; ++k) {
                int sampled_bid = block_dist(rng);
                for (size_t idx : block_order_indices[sampled_bid]) {
                    const auto& ord = completed_orders[idx];
                    b_tot++;
                    if (ord.l3_filled) b_l3++;
                    if (ord.naive_touch_filled) {
                        b_touch++;
                        if (ord.l3_filled) {
                            double lead_ms = (static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.naive_touch_fill_ts)) / 1e6;
                            b_sum_lead_ms += lead_ms;
                            b_lead_count++;
                        }
                    }
                }
            }

            if (b_tot > 0) {
                double l3_r = 100.0 * b_l3 / b_tot;
                double touch_r = 100.0 * b_touch / b_tot;
                bs_l3_rate.push_back(l3_r);
                bs_touch_rate.push_back(touch_r);
                if (b_l3 > 0) {
                    bs_overstate.push_back(static_cast<double>(b_touch) / b_l3);
                }
                if (b_lead_count > 0) {
                    bs_lead_time.push_back(b_sum_lead_ms / b_lead_count);
                }
            }
        }

        auto get_ci = [](std::vector<double>& vec, double point) -> BootstrapCI {
            if (vec.empty()) return {point, point, point};
            std::sort(vec.begin(), vec.end());
            size_t n = vec.size();
            return {point, vec[n * 25 / 1000], vec[n * 975 / 1000]};
        };

        double point_l3 = res.total_orders > 0 ? (100.0 * res.l3_fills / res.total_orders) : 0.0;
        double point_touch = res.total_orders > 0 ? (100.0 * res.touch_fills / res.total_orders) : 0.0;
        double point_overstate = res.l3_fills > 0 ? (static_cast<double>(res.touch_fills) / res.l3_fills) : 0.0;
        double point_lead = 0.0;
        size_t common_fills = 0;
        double sum_lead = 0.0;
        for (const auto& ord : completed_orders) {
            if (ord.l3_filled && ord.naive_touch_filled) {
                common_fills++;
                sum_lead += (static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.naive_touch_fill_ts)) / 1e6;
            }
        }
        if (common_fills > 0) point_lead = sum_lead / common_fills;

        res.l3_fill_rate_ci = get_ci(bs_l3_rate, point_l3);
        res.touch_fill_rate_ci = get_ci(bs_touch_rate, point_touch);
        res.overstatement_ratio_ci = get_ci(bs_overstate, point_overstate);
        res.mean_lead_time_ms_ci = get_ci(bs_lead_time, point_lead);

        // 6. Queue depth bucketing
        struct BucketDef {
            std::string label;
            uint32_t min_q;
            uint32_t max_q;
        };
        std::vector<BucketDef> bdefs = {
            {"Shallow (<=200 sh)", 0, 200},
            {"Medium (201-1k sh)", 201, 1000},
            {"Deep (1001-5k sh)", 1001, 5000},
            {"Very Deep (>5k sh)", 5001, 10000000}
        };

        for (const auto& bdef : bdefs) {
            QueueDepthBucketStats bstat{};
            bstat.label = bdef.label;
            double sum_depth = 0.0;
            double sum_err_p = 0.0;
            double sum_abs_p = 0.0;
            double sum_err_pr = 0.0;
            double sum_abs_pr = 0.0;
            uint64_t total_samples = 0;

            for (const auto& ord : completed_orders) {
                if (ord.initial_queue_ahead >= bdef.min_q && ord.initial_queue_ahead <= bdef.max_q) {
                    bstat.sample_count++;
                    sum_depth += ord.initial_queue_ahead;
                    sum_err_p += ord.sum_err_pess;
                    sum_abs_p += ord.sum_abs_err_pess;
                    sum_err_pr += ord.sum_err_prob;
                    sum_abs_pr += ord.sum_abs_err_prob;
                    total_samples += ord.sample_count;
                }
            }

            if (bstat.sample_count > 0) {
                bstat.avg_depth_ahead = sum_depth / bstat.sample_count;
            }
            if (total_samples > 0) {
                bstat.pess_bias_sh = sum_err_p / total_samples;
                bstat.pess_mae_sh = sum_abs_p / total_samples;
                bstat.pess_rel_err_pct = (bstat.avg_depth_ahead > 0) ? (bstat.pess_mae_sh / bstat.avg_depth_ahead * 100.0) : 0.0;
                bstat.prob_bias_sh = sum_err_pr / total_samples;
                bstat.prob_mae_sh = sum_abs_pr / total_samples;
                bstat.prob_rel_err_pct = (bstat.avg_depth_ahead > 0) ? (bstat.prob_mae_sh / bstat.avg_depth_ahead * 100.0) : 0.0;
            }
            res.depth_buckets.push_back(bstat);
        }

        return res;
    }

private:
    std::string filepath_;
    uint64_t delta_t_ns_{0};
    uint64_t ttl_ns_{5'000'000'000ULL};
    double obi_threshold_{0.0};
};

int main(int argc, char* argv[]) {
    std::string filepath = "data/fixture_3symbols_full.bin.gz";
    if (argc > 1) {
        filepath = argv[1];
    }

    std::cout << "========================================================================================\n";
    std::cout << "STAGE 3: STATISTICALLY RIGOROUS QUEUE ESTIMATION & LATENCY SIMULATOR\n";
    std::cout << "Dataset: " << filepath << " (Regular Hours 09:30 - 16:00 ET Strictly Filtered)\n";
    std::cout << "Definition: Naive touch fill assumes a limit order fills immediately and in full\n";
    std::cout << "            upon any trade at or through its limit price, disregarding queue depth.\n";
    std::cout << "========================================================================================\n\n";

    // Required sweeps: Delta t = 0, 50, 100, 250, 500 us
    std::vector<uint64_t> latencies_ns = {
        0,           // 0 us (co-located zero-transit)
        50'000,      // 50 us (metro fiber)
        100'000,     // 100 us (standard broker gateway)
        250'000,     // 250 us (inter-exchange transit)
        500'000      // 500 us (regional transit)
    };

    std::vector<LatencyRunResult> results;
    for (uint64_t dt : latencies_ns) {
        std::cout << "Running replay for Delta t = " << (dt / 1'000) << " us ... " << std::flush;
        auto start = std::chrono::steady_clock::now();
        Stage3SimulationEngine engine(filepath, dt, 5'000'000'000ULL);
        LatencyRunResult res = engine.run();
        auto end = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(end - start).count();
        std::cout << "Done (" << std::fixed << std::setprecision(2) << elapsed_s << " s, "
                  << res.total_orders << " orders, " << res.l3_fills << " true L3 fills)\n";
        results.push_back(res);
    }

    std::cout << "\n====================================================================================================================================================\n";
    std::cout << "HEADLINE TABLE: FILL OVERSTATEMENT & ADVERSE SELECTION MARKOUT ACROSS WIRE LATENCY (Delta t)\n";
    std::cout << "Strategy: Post 100 shares at Best Bid when OBI > 0.0, 5s TTL, Regular Hours 09:30-16:00 ET\n";
    std::cout << "====================================================================================================================================================\n";
    std::cout << std::left << std::setw(10) << "Delta t"
              << std::right << std::setw(10) << "Orders"
              << std::setw(10) << "L3 Fills"
              << std::setw(22) << "True L3 Rate (95% CI)"
              << std::setw(24) << "Touch Rate (95% CI)"
              << std::setw(24) << "Touch Overstate (95% CI)"
              << std::setw(18) << "Markout 1s (bps)"
              << std::setw(18) << "Markout 5s (bps)\n";
    std::cout << "----------------------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& r : results) {
        std::string dt_str = std::to_string(r.delta_t_ns / 1000) + " us";
        std::ostringstream ss_l3, ss_touch, ss_over;
        ss_l3 << std::fixed << std::setprecision(2) << r.l3_fill_rate_ci.point_est << "% ["
              << r.l3_fill_rate_ci.ci_lo << "%-" << r.l3_fill_rate_ci.ci_hi << "%]";
        ss_touch << std::fixed << std::setprecision(2) << r.touch_fill_rate_ci.point_est << "% ["
                 << r.touch_fill_rate_ci.ci_lo << "%-" << r.touch_fill_rate_ci.ci_hi << "%]";
        ss_over << std::fixed << std::setprecision(2) << r.overstatement_ratio_ci.point_est << "x ["
                << r.overstatement_ratio_ci.ci_lo << "x-" << r.overstatement_ratio_ci.ci_hi << "x]";

        std::cout << std::left << std::setw(10) << dt_str
                  << std::right << std::setw(10) << r.total_orders
                  << std::setw(10) << r.l3_fills
                  << std::setw(22) << ss_l3.str()
                  << std::setw(24) << ss_touch.str()
                  << std::setw(24) << ss_over.str()
                  << std::setw(18) << std::fixed << std::setprecision(2) << r.mean_markout_1s_bps
                  << std::setw(18) << std::fixed << std::setprecision(2) << r.mean_markout_5s_bps << "\n";
    }
    std::cout << "====================================================================================================================================================\n\n";

    std::cout << "========================================================================================================================\n";
    std::cout << "LEVEL-2 QUEUE ESTIMATION ACCURACY BY INITIAL QUEUE DEPTH BUCKET (Delta t = 0 us)\n";
    std::cout << "Comparing Pessimistic (cancels ahead ignored) vs Probabilistic (proportional cancel discount)\n";
    std::cout << "========================================================================================================================\n";
    std::cout << std::left << std::setw(22) << "Depth Bucket"
              << std::right << std::setw(10) << "Orders"
              << std::setw(14) << "Avg Depth"
              << std::setw(15) << "Pess Bias (sh)"
              << std::setw(15) << "Pess MAE (sh)"
              << std::setw(15) << "Pess Rel Err"
              << std::setw(15) << "Prob Bias (sh)"
              << std::setw(15) << "Prob MAE (sh)"
              << std::setw(15) << "Prob Rel Err\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& b : results[0].depth_buckets) {
        std::cout << std::left << std::setw(22) << b.label
                  << std::right << std::setw(10) << b.sample_count
                  << std::setw(14) << std::fixed << std::setprecision(1) << b.avg_depth_ahead
                  << std::setw(15) << std::fixed << std::setprecision(1) << b.pess_bias_sh
                  << std::setw(15) << std::fixed << std::setprecision(1) << b.pess_mae_sh
                  << std::setw(14) << std::fixed << std::setprecision(1) << b.pess_rel_err_pct << "%"
                  << std::setw(15) << std::fixed << std::setprecision(1) << b.prob_bias_sh
                  << std::setw(15) << std::fixed << std::setprecision(1) << b.prob_mae_sh
                  << std::setw(14) << std::fixed << std::setprecision(1) << b.prob_rel_err_pct << "%\n";
    }
    std::cout << "========================================================================================================================\n\n";

    // Sensitivity & Variant Disclosure (Multiple Testing Control)
    std::cout << "========================================================================================================================\n";
    std::cout << "PARAMETER SENSITIVITY & VARIANT DISCLOSURE (Full Multiple-Testing Space Disclosed)\n";
    std::cout << "========================================================================================================================\n";
    struct VariantDef {
        std::string name;
        uint64_t ttl_ns;
        double obi_thr;
    };
    std::vector<VariantDef> variants = {
        {"Primary Baseline (5s TTL, OBI > 0.0)", 5'000'000'000ULL, 0.0},
        {"Variant A: Fast Cancel (1s TTL, OBI > 0.0)", 1'000'000'000ULL, 0.0},
        {"Variant B: Patient Maker (10s TTL, OBI > 0.0)", 10'000'000'000ULL, 0.0},
        {"Variant C: Selective Imbalance (5s TTL, OBI > 0.20)", 5'000'000'000ULL, 0.20},
        {"Variant D: Unconditional Post (5s TTL, no OBI filter)", 5'000'000'000ULL, -1.0}
    };

    std::cout << std::left << std::setw(55) << "Strategy Variant Description"
              << std::right << std::setw(10) << "Orders"
              << std::setw(10) << "L3 Fills"
              << std::setw(14) << "True L3 Rate"
              << std::setw(14) << "Touch Rate"
              << std::setw(18) << "Touch Overstate"
              << std::setw(14) << "Markout 5s\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& var : variants) {
        Stage3SimulationEngine v_engine(filepath, 0, var.ttl_ns, var.obi_thr);
        auto v_res = v_engine.run();
        double l3_r = v_res.total_orders > 0 ? (100.0 * v_res.l3_fills / v_res.total_orders) : 0.0;
        double t_r = v_res.total_orders > 0 ? (100.0 * v_res.touch_fills / v_res.total_orders) : 0.0;
        double over = v_res.l3_fills > 0 ? (static_cast<double>(v_res.touch_fills) / v_res.l3_fills) : 0.0;

        std::cout << std::left << std::setw(55) << var.name
                  << std::right << std::setw(10) << v_res.total_orders
                  << std::setw(10) << v_res.l3_fills
                  << std::setw(13) << std::fixed << std::setprecision(2) << l3_r << "%"
                  << std::setw(13) << std::fixed << std::setprecision(2) << t_r << "%"
                  << std::setw(17) << std::fixed << std::setprecision(2) << over << "x"
                  << std::setw(13) << std::fixed << std::setprecision(2) << v_res.mean_markout_5s_bps << " bps\n";
    }
    std::cout << "========================================================================================================================\n";

    return 0;
}
