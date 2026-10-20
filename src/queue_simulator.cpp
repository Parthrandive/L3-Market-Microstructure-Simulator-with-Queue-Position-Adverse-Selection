#include <iostream>
#include <iomanip>
#include <vector>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <string>
#include <cmath>
#include "orderbook/itch_parser.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/queue_tracker.hpp"
#include "orderbook/ring_buffer.hpp"

using namespace l3;

// Simulated order for the passive strategy
struct SimOrder {
    uint64_t sim_id{0};
    uint16_t stock_locate{0};
    Side side{Side::Buy};
    uint32_t price{0};
    uint32_t shares{100};
    uint64_t submit_ts_ns{0};
    uint64_t arrival_ts_ns{0};
    uint64_t cancel_ts_ns{0};
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
};

// Natural market order tracked from ITCH feed
struct NaturalOrder {
    uint64_t order_ref{0};
    uint16_t stock_locate{0};
    Side side{Side::Buy};
    uint32_t price{0};
    uint32_t shares{0};
    uint64_t arrival_ts{0};
    uint32_t initial_queue_ahead{0};

    bool l3_filled{false};
    bool l3_canceled{false};
    uint64_t l3_fill_ts{0};

    L2QueueState pessimistic{};
    L2QueueState probabilistic{};
    bool naive_touch_filled{false};
    uint64_t naive_touch_fill_ts{0};

    uint64_t sample_count{0};
    double sum_err_pess{0.0};
    double sum_abs_err_pess{0.0};
    double sum_err_prob{0.0};
    double sum_abs_err_prob{0.0};
};

struct LatencyExperimentResult {
    uint64_t delta_t_ns{0};
    size_t total_orders{0};
    size_t l3_fills{0};
    size_t naive_touch_fills{0};
    size_t pessimistic_fills{0};
    size_t probabilistic_fills{0};
    size_t touch_false_positives{0};
    size_t pess_false_positives{0};
    size_t prob_false_positives{0};
    double avg_initial_queue_ahead{0.0};
    double pess_mae_shares{0.0};
    double prob_mae_shares{0.0};
    double pess_bias_shares{0.0};
    double prob_bias_shares{0.0};
    double mean_touch_fill_lead_time_ms{0.0};
};

struct NaturalFlowResult {
    size_t total_sampled{0};
    size_t l3_fills{0};
    size_t naive_touch_fills{0};
    size_t pess_fills{0};
    size_t prob_fills{0};
    size_t touch_false_positives{0};
    double avg_queue_ahead{0.0};
    double pess_bias_shares{0.0};
    double pess_mae_shares{0.0};
    double prob_bias_shares{0.0};
    double prob_mae_shares{0.0};
    double mean_touch_lead_ms{0.0};
};

class Stage3ReplayEngine {
public:
    Stage3ReplayEngine(const std::string& path, uint64_t delta_t_ns, bool track_natural = false)
        : filepath_(path), delta_t_ns_(delta_t_ns), track_natural_(track_natural) {}

    std::pair<LatencyExperimentResult, NaturalFlowResult> run() {
        gzFile gz = gzopen(filepath_.c_str(), "rb");
        if (!gz) {
            std::cerr << "Failed to open " << filepath_ << "\n";
            return {};
        }

        std::unordered_map<uint16_t, std::string> locate_to_sym;
        std::unordered_map<uint16_t, OrderBook> books;
        std::vector<SimOrder> completed_orders;
        std::deque<SimOrder> in_flight_orders;
        std::vector<SimOrder*> resting_sim_orders;

        std::unordered_map<uint64_t, NaturalOrder> active_natural_orders;
        std::vector<NaturalOrder> completed_natural_orders;

        constexpr size_t BUF_SIZE = 65536;
        std::vector<uint8_t> buffer(BUF_SIZE);
        size_t leftover = 0;
        uint64_t current_ts_ns = 0;
        uint64_t next_sim_id = 900'000'000'000ULL;
        uint64_t last_order_submit_ts = 0;
        size_t natural_sample_stride = 0;

        constexpr uint64_t MARKET_OPEN_NS = 9ULL * 3600ULL * 1'000'000'000ULL + 30ULL * 60ULL * 1'000'000'000ULL;
        constexpr uint64_t MARKET_CLOSE_NS = 16ULL * 3600ULL * 1'000'000'000ULL;
        constexpr uint64_t ORDER_INTERVAL_NS = 1'000'000'000ULL;
        constexpr uint64_t ORDER_TTL_NS = 5'000'000'000ULL;

        auto process_event = [&](char msg_type, uint16_t locate, uint64_t ts,
                                 uint64_t ref, uint32_t shares, uint32_t price, Side side, uint64_t new_ref) {
            current_ts_ns = ts;

            // 1. Process in-flight orders arriving after wire delay
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

                    completed_orders.push_back(ord);
                    resting_sim_orders.push_back(&completed_orders.back());
                }
            }

            // 2. TTL expiration for resting simulated orders
            for (auto it = resting_sim_orders.begin(); it != resting_sim_orders.end();) {
                SimOrder* ord = *it;
                if (!ord->l3_filled && !ord->l3_canceled && current_ts_ns >= ord->cancel_ts_ns) {
                    auto b_it = books.find(ord->stock_locate);
                    if (b_it != books.end()) {
                        b_it->second.cancel_order(ord->sim_id, ord->shares);
                    }
                    ord->l3_canceled = true;
                    it = resting_sim_orders.erase(it);
                } else if (ord->l3_filled || ord->l3_canceled) {
                    it = resting_sim_orders.erase(it);
                } else {
                    ++it;
                }
            }

            // 3. Process ITCH market message
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

                // If execution: check simulated resting orders
                if ((msg_type == 'E' || msg_type == 'C') && is_target_level) {
                    for (SimOrder* ord : resting_sim_orders) {
                        if (ord->stock_locate == locate && ord->price == price && ord->side == side && !ord->l3_filled) {
                            if (!ord->naive_touch_filled) {
                                ord->naive_touch_filled = true;
                                ord->naive_touch_fill_ts = current_ts_ns;
                            }

                            auto qpos_before = book.get_queue_position(ord->sim_id);
                            if (qpos_before.exists) {
                                bool ahead = book.is_order_ahead(ref, ord->sim_id);
                                if (qpos_before.shares_ahead == 0) {
                                    ord->l3_filled = true;
                                    ord->l3_fill_ts = current_ts_ns;
                                } else if (!ahead && ref != ord->sim_id) {
                                    ord->l3_filled = true;
                                    ord->l3_fill_ts = current_ts_ns;
                                } else if (ahead && shares > qpos_before.shares_ahead) {
                                    ord->l3_filled = true;
                                    ord->l3_fill_ts = current_ts_ns;
                                }
                            }

                            ord->pessimistic.on_execution(shares, current_ts_ns);
                            ord->probabilistic.on_execution(shares, current_ts_ns);
                        }
                    }

                    // Check natural orders
                    if (track_natural_) {
                        for (auto& [nord_ref, nord] : active_natural_orders) {
                            if (nord.stock_locate == locate && nord.price == price && nord.side == side && !nord.l3_filled) {
                                if (!nord.naive_touch_filled) {
                                    nord.naive_touch_filled = true;
                                    nord.naive_touch_fill_ts = current_ts_ns;
                                }
                                if (ref == nord_ref) {
                                    nord.l3_filled = true;
                                    nord.l3_fill_ts = current_ts_ns;
                                }
                                nord.pessimistic.on_execution(shares, current_ts_ns);
                                nord.probabilistic.on_execution(shares, current_ts_ns);
                            }
                        }
                    }
                }

                // If cancel: check natural orders
                if (track_natural_ && (msg_type == 'X' || msg_type == 'D') && is_target_level) {
                    auto nat_it = active_natural_orders.find(ref);
                    if (nat_it != active_natural_orders.end()) {
                        nat_it->second.l3_canceled = true;
                    }
                }

                // Apply mutation to book
                switch (msg_type) {
                    case 'A':
                    case 'F': {
                        // Sample natural orders that join the BBO
                        if (track_natural_ && current_ts_ns >= MARKET_OPEN_NS && current_ts_ns < MARKET_CLOSE_NS) {
                            if (side == Side::Buy) {
                                auto bbo = book.best_bid();
                                if (bbo && bbo->first == price) {
                                    natural_sample_stride++;
                                    if (natural_sample_stride % 10 == 0 && active_natural_orders.size() < 10000) {
                                        NaturalOrder nord{};
                                        nord.order_ref = ref;
                                        nord.stock_locate = locate;
                                        nord.side = side;
                                        nord.price = price;
                                        nord.shares = shares;
                                        nord.arrival_ts = current_ts_ns;
                                        nord.initial_queue_ahead = bbo->second;
                                        nord.pessimistic.init(bbo->second, shares);
                                        nord.probabilistic.init(bbo->second, shares);
                                        active_natural_orders[ref] = nord;
                                    }
                                }
                            }
                        }
                        book.add_order(ref, side, shares, price);
                        break;
                    }
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
                    for (SimOrder* ord : resting_sim_orders) {
                        if (ord->stock_locate == locate && ord->price == price && ord->side == side && !ord->l3_filled) {
                            auto qpos_after = book.get_queue_position(ord->sim_id);
                            uint32_t depth_after = qpos_after.exists ? qpos_after.level_total_shares : 0;
                            ord->pessimistic.on_cancel_pessimistic(depth_after);
                            ord->probabilistic.on_cancel_probabilistic(shares, depth_before, depth_after);
                        }
                    }

                    if (track_natural_) {
                        for (auto& [nord_ref, nord] : active_natural_orders) {
                            if (nord.stock_locate == locate && nord.price == price && nord.side == side && !nord.l3_filled) {
                                auto qpos_after = book.get_queue_position(nord_ref);
                                uint32_t depth_after = qpos_after.exists ? qpos_after.level_total_shares : 0;
                                nord.pessimistic.on_cancel_pessimistic(depth_after);
                                nord.probabilistic.on_cancel_probabilistic(shares, depth_before, depth_after);
                            }
                        }
                    }
                }

                // Sample error stats for resting orders
                for (SimOrder* ord : resting_sim_orders) {
                    if (ord->stock_locate == locate && ord->price == price && ord->side == side && !ord->l3_filled) {
                        auto qpos = book.get_queue_position(ord->sim_id);
                        if (qpos.exists) {
                            double q_true = static_cast<double>(qpos.shares_ahead);
                            ord->sample_count++;
                            double err_pess = ord->pessimistic.queue_ahead - q_true;
                            ord->sum_err_pess += err_pess;
                            ord->sum_abs_err_pess += std::abs(err_pess);

                            double err_prob = ord->probabilistic.queue_ahead - q_true;
                            ord->sum_err_prob += err_prob;
                            ord->sum_abs_err_prob += std::abs(err_prob);
                        }
                    }
                }

                if (track_natural_) {
                    for (auto it = active_natural_orders.begin(); it != active_natural_orders.end();) {
                        NaturalOrder& nord = it->second;
                        if (nord.stock_locate == locate && nord.price == price && nord.side == side) {
                            auto qpos = book.get_queue_position(nord.order_ref);
                            if (qpos.exists && !nord.l3_filled) {
                                double q_true = static_cast<double>(qpos.shares_ahead);
                                nord.sample_count++;
                                double err_pess = nord.pessimistic.queue_ahead - q_true;
                                nord.sum_err_pess += err_pess;
                                nord.sum_abs_err_pess += std::abs(err_pess);

                                double err_prob = nord.probabilistic.queue_ahead - q_true;
                                nord.sum_err_prob += err_prob;
                                nord.sum_abs_err_prob += std::abs(err_prob);
                            }
                        }
                        if (nord.l3_filled || nord.l3_canceled) {
                            completed_natural_orders.push_back(nord);
                            it = active_natural_orders.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                // 4. Submit simulated strategy orders
                if (current_ts_ns >= MARKET_OPEN_NS && current_ts_ns < MARKET_CLOSE_NS) {
                    if (current_ts_ns - last_order_submit_ts >= ORDER_INTERVAL_NS) {
                        auto best_b = book.best_bid();
                        if (best_b.has_value()) {
                            SimOrder new_ord{};
                            new_ord.sim_id = ++next_sim_id;
                            new_ord.stock_locate = locate;
                            new_ord.side = Side::Buy;
                            new_ord.price = best_b->first;
                            new_ord.shares = 100;
                            new_ord.submit_ts_ns = current_ts_ns;
                            new_ord.arrival_ts_ns = current_ts_ns + delta_t_ns_;
                            new_ord.cancel_ts_ns = new_ord.arrival_ts_ns + ORDER_TTL_NS;

                            in_flight_orders.push_back(new_ord);
                            last_order_submit_ts = current_ts_ns;
                        }
                    }
                }
            }
        };

        // Streaming loop
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
                        .order_pool_capacity = 65536,
                        .level_pool_capacity = 8192,
                        .order_map_capacity = 131072
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

        // Aggregate simulated statistics
        LatencyExperimentResult res{};
        res.delta_t_ns = delta_t_ns_;
        res.total_orders = completed_orders.size();

        double sum_q_init = 0.0;
        double tot_err_pess = 0.0;
        double tot_abs_err_pess = 0.0;
        double tot_err_prob = 0.0;
        double tot_abs_err_prob = 0.0;
        uint64_t total_samples = 0;
        double sum_lead_time_ms = 0.0;
        size_t common_fills = 0;

        for (const auto& ord : completed_orders) {
            sum_q_init += ord.initial_queue_ahead;
            if (ord.l3_filled) res.l3_fills++;
            if (ord.naive_touch_filled) {
                res.naive_touch_fills++;
                if (!ord.l3_filled) res.touch_false_positives++;
            }
            if (ord.pessimistic.filled) {
                res.pessimistic_fills++;
                if (!ord.l3_filled) res.pess_false_positives++;
            }
            if (ord.probabilistic.filled) {
                res.probabilistic_fills++;
                if (!ord.l3_filled) res.prob_false_positives++;
            }

            tot_err_pess += ord.sum_err_pess;
            tot_abs_err_pess += ord.sum_abs_err_pess;
            tot_err_prob += ord.sum_err_prob;
            tot_abs_err_prob += ord.sum_abs_err_prob;
            total_samples += ord.sample_count;

            if (ord.l3_filled && ord.naive_touch_filled) {
                common_fills++;
                double lead_ns = static_cast<double>(ord.l3_fill_ts) - static_cast<double>(ord.naive_touch_fill_ts);
                sum_lead_time_ms += (lead_ns / 1'000'000.0);
            }
        }

        if (res.total_orders > 0) {
            res.avg_initial_queue_ahead = sum_q_init / static_cast<double>(res.total_orders);
        }
        if (total_samples > 0) {
            res.pess_bias_shares = tot_err_pess / static_cast<double>(total_samples);
            res.pess_mae_shares = tot_abs_err_pess / static_cast<double>(total_samples);
            res.prob_bias_shares = tot_err_prob / static_cast<double>(total_samples);
            res.prob_mae_shares = tot_abs_err_prob / static_cast<double>(total_samples);
        }
        if (common_fills > 0) {
            res.mean_touch_fill_lead_time_ms = sum_lead_time_ms / static_cast<double>(common_fills);
        }

        // Aggregate natural statistics
        NaturalFlowResult nat_res{};
        nat_res.total_sampled = completed_natural_orders.size();
        double nat_sum_q = 0.0;
        double nat_err_pess = 0.0;
        double nat_abs_pess = 0.0;
        double nat_err_prob = 0.0;
        double nat_abs_prob = 0.0;
        uint64_t nat_samples = 0;
        double nat_lead_ms = 0.0;
        size_t nat_common = 0;

        for (const auto& nord : completed_natural_orders) {
            nat_sum_q += nord.initial_queue_ahead;
            if (nord.l3_filled) nat_res.l3_fills++;
            if (nord.naive_touch_filled) {
                nat_res.naive_touch_fills++;
                if (!nord.l3_filled) nat_res.touch_false_positives++;
            }
            if (nord.pessimistic.filled) nat_res.pess_fills++;
            if (nord.probabilistic.filled) nat_res.prob_fills++;

            nat_err_pess += nord.sum_err_pess;
            nat_abs_pess += nord.sum_abs_err_pess;
            nat_err_prob += nord.sum_err_prob;
            nat_abs_prob += nord.sum_abs_err_prob;
            nat_samples += nord.sample_count;

            if (nord.l3_filled && nord.naive_touch_filled) {
                nat_common++;
                double lead_ns = static_cast<double>(nord.l3_fill_ts) - static_cast<double>(nord.naive_touch_fill_ts);
                nat_lead_ms += (lead_ns / 1'000'000.0);
            }
        }

        if (nat_res.total_sampled > 0) {
            nat_res.avg_queue_ahead = nat_sum_q / static_cast<double>(nat_res.total_sampled);
        }
        if (nat_samples > 0) {
            nat_res.pess_bias_shares = nat_err_pess / static_cast<double>(nat_samples);
            nat_res.pess_mae_shares = nat_abs_pess / static_cast<double>(nat_samples);
            nat_res.prob_bias_shares = nat_err_prob / static_cast<double>(nat_samples);
            nat_res.prob_mae_shares = nat_abs_prob / static_cast<double>(nat_samples);
        }
        if (nat_common > 0) {
            nat_res.mean_touch_lead_ms = nat_lead_ms / static_cast<double>(nat_common);
        }

        return {res, nat_res};
    }

private:
    std::string filepath_;
    uint64_t delta_t_ns_{0};
    bool track_natural_{false};
};

int main(int argc, char* argv[]) {
    std::string filepath = "data/fixture_3symbols_full.bin.gz";
    if (argc > 1) {
        filepath = argv[1];
    }

    std::cout << "========================================================================\n";
    std::cout << "STAGE 3: QUEUE POSITION ESTIMATION & WIRE LATENCY SENSITIVITY BENCHMARK\n";
    std::cout << "Dataset: " << filepath << "\n";
    std::cout << "========================================================================\n\n";

    std::vector<uint64_t> latencies_ns = {
        0,           // 0 us
        5'000,       // 5 us
        20'000,      // 20 us
        50'000,      // 50 us
        100'000,     // 100 us
        500'000      // 500 us
    };

    std::vector<LatencyExperimentResult> results;
    NaturalFlowResult nat_result{};

    for (size_t i = 0; i < latencies_ns.size(); ++i) {
        uint64_t dt = latencies_ns[i];
        bool track_nat = (i == 0); // Track natural flow on first pass
        std::cout << "Running replay for Delta t = " << (dt / 1'000) << " us ... " << std::flush;
        auto start = std::chrono::steady_clock::now();
        Stage3ReplayEngine engine(filepath, dt, track_nat);
        auto [res, nat_res] = engine.run();
        auto end = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(end - start).count();
        std::cout << "Done (" << std::fixed << std::setprecision(2) << elapsed_s << " s)\n";
        results.push_back(res);
        if (track_nat) {
            nat_result = nat_res;
        }
    }

    std::cout << "\n------------------------------------------------------------------------------------------------------------------------\n";
    std::cout << "TABLE 1: FILL OVERSTATEMENT & MODEL DIVERGENCE ACROSS WIRE LATENCY (Delta t)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(10) << "Delta t"
              << std::right << std::setw(10) << "Orders"
              << std::setw(12) << "True L3"
              << std::setw(12) << "L2 Pess"
              << std::setw(12) << "L2 Prob"
              << std::setw(14) << "Naive Touch"
              << std::setw(15) << "Touch Overstate"
              << std::setw(15) << "False Pos Fills"
              << std::setw(16) << "Mean Lead (ms)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& r : results) {
        double l3_rate = (r.total_orders > 0) ? (100.0 * r.l3_fills / r.total_orders) : 0.0;
        double touch_rate = (r.total_orders > 0) ? (100.0 * r.naive_touch_fills / r.total_orders) : 0.0;
        double pess_rate = (r.total_orders > 0) ? (100.0 * r.pessimistic_fills / r.total_orders) : 0.0;
        double prob_rate = (r.total_orders > 0) ? (100.0 * r.probabilistic_fills / r.total_orders) : 0.0;
        double overstate_ratio = (r.l3_fills > 0) ? (static_cast<double>(r.naive_touch_fills) / r.l3_fills) : 0.0;

        std::string dt_str = std::to_string(r.delta_t_ns / 1000) + " us";
        std::cout << std::left << std::setw(10) << dt_str
                  << std::right << std::setw(10) << r.total_orders
                  << std::setw(11) << std::fixed << std::setprecision(1) << l3_rate << "%"
                  << std::setw(11) << std::fixed << std::setprecision(1) << pess_rate << "%"
                  << std::setw(11) << std::fixed << std::setprecision(1) << prob_rate << "%"
                  << std::setw(13) << std::fixed << std::setprecision(1) << touch_rate << "%"
                  << std::setw(14) << std::fixed << std::setprecision(2) << overstate_ratio << "x"
                  << std::setw(15) << r.touch_false_positives
                  << std::setw(16) << std::fixed << std::setprecision(2) << r.mean_touch_fill_lead_time_ms << "\n";
    }
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n\n";

    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
    std::cout << "TABLE 2: LEVEL 2 QUEUE ESTIMATION ACCURACY (vs GROUND TRUTH L3 QUEUE AHEAD)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(10) << "Delta t"
              << std::right << std::setw(16) << "Avg Depth Ahead"
              << std::setw(18) << "Pess Bias (sh)"
              << std::setw(18) << "Pess MAE (sh)"
              << std::setw(18) << "Prob Bias (sh)"
              << std::setw(18) << "Prob MAE (sh)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& r : results) {
        std::string dt_str = std::to_string(r.delta_t_ns / 1000) + " us";
        std::cout << std::left << std::setw(10) << dt_str
                  << std::right << std::setw(16) << std::fixed << std::setprecision(1) << r.avg_initial_queue_ahead
                  << std::setw(18) << std::fixed << std::setprecision(1) << r.pess_bias_shares
                  << std::setw(18) << std::fixed << std::setprecision(1) << r.pess_mae_shares
                  << std::setw(18) << std::fixed << std::setprecision(1) << r.prob_bias_shares
                  << std::setw(18) << std::fixed << std::setprecision(1) << r.prob_mae_shares << "\n";
    }
    std::cout << "------------------------------------------------------------------------------------------------------------------------\n\n";

    if (nat_result.total_sampled > 0) {
        std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
        std::cout << "TABLE 3: NATURAL MARKET ORDER FLOW EVALUATION (Sampled Real ITCH BBO Orders)\n";
        std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
        double nat_l3_rate = 100.0 * nat_result.l3_fills / nat_result.total_sampled;
        double nat_touch_rate = 100.0 * nat_result.naive_touch_fills / nat_result.total_sampled;
        double nat_pess_rate = 100.0 * nat_result.pess_fills / nat_result.total_sampled;
        double nat_prob_rate = 100.0 * nat_result.prob_fills / nat_result.total_sampled;
        double nat_overstate = (nat_result.l3_fills > 0) ? (static_cast<double>(nat_result.naive_touch_fills) / nat_result.l3_fills) : 0.0;

        std::cout << "Sampled Real BBO Orders:      " << nat_result.total_sampled << "\n";
        std::cout << "Average Initial Queue Depth:  " << std::fixed << std::setprecision(1) << nat_result.avg_queue_ahead << " shares\n";
        std::cout << "True L3 Fill Rate:            " << std::fixed << std::setprecision(2) << nat_l3_rate << "% (" << nat_result.l3_fills << " fills)\n";
        std::cout << "L2 Pessimistic Fill Rate:     " << std::fixed << std::setprecision(2) << nat_pess_rate << "% (" << nat_result.pess_fills << " fills)\n";
        std::cout << "L2 Probabilistic Fill Rate:   " << std::fixed << std::setprecision(2) << nat_prob_rate << "% (" << nat_result.prob_fills << " fills)\n";
        std::cout << "Naive Touch Fill Rate:        " << std::fixed << std::setprecision(2) << nat_touch_rate << "% (" << nat_result.naive_touch_fills << " fills)\n";
        std::cout << "Naive Touch Overstatement:    " << std::fixed << std::setprecision(2) << nat_overstate << "x\n";
        std::cout << "False Positive Touch Fills:   " << nat_result.touch_false_positives << " orders\n";
        std::cout << "Mean Fill Time Lead (Touch):  " << std::fixed << std::setprecision(2) << nat_result.mean_touch_lead_ms << " ms ahead of true fill\n";
        std::cout << "Pessimistic Model MAE:        " << std::fixed << std::setprecision(1) << nat_result.pess_mae_shares << " shares (Bias: +" << nat_result.pess_bias_shares << " sh)\n";
        std::cout << "Probabilistic Model MAE:      " << std::fixed << std::setprecision(1) << nat_result.prob_mae_shares << " shares (Bias: +" << nat_result.prob_bias_shares << " sh)\n";
        std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
    }

    return 0;
}
