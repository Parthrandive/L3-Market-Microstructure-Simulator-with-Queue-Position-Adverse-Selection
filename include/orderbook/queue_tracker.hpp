#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <optional>
#include <string>
#include "orderbook/types.hpp"
#include "orderbook/order_book.hpp"

namespace l3 {

enum class QueueModelType : uint8_t {
    L3GroundTruth,
    L2Pessimistic,
    L2Probabilistic,
    NaiveTouch
};

struct L2QueueState {
    double queue_ahead{0.0};
    uint32_t remaining_order_shares{0};
    bool filled{false};
    uint64_t fill_timestamp_ns{0};

    void init(uint32_t shares_ahead_at_entry, uint32_t order_shares) noexcept {
        queue_ahead = static_cast<double>(shares_ahead_at_entry);
        remaining_order_shares = order_shares;
        filled = false;
        fill_timestamp_ns = 0;
    }

    void on_execution(uint32_t executed_shares, uint64_t timestamp_ns) noexcept {
        if (filled) return;

        if (static_cast<double>(executed_shares) < queue_ahead) {
            queue_ahead -= executed_shares;
        } else {
            double remaining_exec = static_cast<double>(executed_shares) - queue_ahead;
            queue_ahead = 0.0;
            if (remaining_exec >= remaining_order_shares) {
                remaining_order_shares = 0;
                filled = true;
                fill_timestamp_ns = timestamp_ns;
            } else {
                remaining_order_shares -= static_cast<uint32_t>(remaining_exec);
            }
        }
    }

    void on_cancel_pessimistic(uint32_t level_depth_after_cancel) noexcept {
        if (filled) return;
        // Pessimistic: cancels ahead do not advance queue priority.
        // Clamp to level depth if level collapsed below estimated queue ahead.
        if (queue_ahead > static_cast<double>(level_depth_after_cancel)) {
            queue_ahead = static_cast<double>(level_depth_after_cancel);
        }
    }

    void on_cancel_probabilistic(uint32_t canceled_shares,
                                 uint32_t level_depth_before_cancel,
                                 uint32_t level_depth_after_cancel) noexcept {
        if (filled) return;
        // Probabilistic: cancels distributed proportionally across level depth
        if (level_depth_before_cancel > 0 && queue_ahead > 0.0) {
            double frac = queue_ahead / static_cast<double>(level_depth_before_cancel);
            if (frac > 1.0) frac = 1.0;
            double cancels_ahead = static_cast<double>(canceled_shares) * frac;
            queue_ahead = std::max(0.0, queue_ahead - cancels_ahead);
        }
        if (queue_ahead > static_cast<double>(level_depth_after_cancel)) {
            queue_ahead = static_cast<double>(level_depth_after_cancel);
        }
    }
};

struct TrackedOrder {
    uint64_t order_ref{0};
    Side side{Side::Buy};
    uint32_t price{0};
    uint32_t original_shares{0};
    uint64_t submit_timestamp_ns{0};
    uint64_t entry_timestamp_ns{0};
    uint32_t initial_queue_ahead{0};

    // Ground Truth L3 State
    uint32_t l3_remaining_shares{0};
    bool l3_filled{false};
    bool l3_canceled{false};
    uint64_t l3_fill_timestamp_ns{0};
    uint64_t l3_cancel_timestamp_ns{0};

    // L2 Models
    L2QueueState pessimistic{};
    L2QueueState probabilistic{};

    // Naive touch model
    bool naive_touch_filled{false};
    uint64_t naive_touch_fill_timestamp_ns{0};

    // Active resting indicator
    bool is_active{true};

    // Error tracking accumulators while resting
    uint64_t update_count{0};
    double sum_error_pessimistic{0.0};
    double sum_abs_error_pessimistic{0.0};
    double max_abs_error_pessimistic{0.0};

    double sum_error_probabilistic{0.0};
    double sum_abs_error_probabilistic{0.0};
    double max_abs_error_probabilistic{0.0};

    void init(uint64_t ref, Side s, uint32_t px, uint32_t shares,
              uint64_t submit_ts, uint64_t entry_ts, uint32_t q_ahead) noexcept {
        order_ref = ref;
        side = s;
        price = px;
        original_shares = shares;
        submit_timestamp_ns = submit_ts;
        entry_timestamp_ns = entry_ts;
        initial_queue_ahead = q_ahead;

        l3_remaining_shares = shares;
        l3_filled = false;
        l3_canceled = false;
        l3_fill_timestamp_ns = 0;
        l3_cancel_timestamp_ns = 0;

        pessimistic.init(q_ahead, shares);
        probabilistic.init(q_ahead, shares);

        naive_touch_filled = false;
        naive_touch_fill_timestamp_ns = 0;

        is_active = true;

        update_count = 0;
        sum_error_pessimistic = 0.0;
        sum_abs_error_pessimistic = 0.0;
        max_abs_error_pessimistic = 0.0;

        sum_error_probabilistic = 0.0;
        sum_abs_error_probabilistic = 0.0;
        max_abs_error_probabilistic = 0.0;
    }

    void record_queue_sample(double q_true) noexcept {
        if (!is_active) return;
        update_count++;

        // Pessimistic error
        double err_pess = pessimistic.queue_ahead - q_true;
        sum_error_pessimistic += err_pess;
        double abs_err_pess = std::abs(err_pess);
        sum_abs_error_pessimistic += abs_err_pess;
        if (abs_err_pess > max_abs_error_pessimistic) {
            max_abs_error_pessimistic = abs_err_pess;
        }

        // Probabilistic error
        double err_prob = probabilistic.queue_ahead - q_true;
        sum_error_probabilistic += err_prob;
        double abs_err_prob = std::abs(err_prob);
        sum_abs_error_probabilistic += abs_err_prob;
        if (abs_err_prob > max_abs_error_probabilistic) {
            max_abs_error_probabilistic = abs_err_prob;
        }
    }
};

struct QueueEvaluationSummary {
    size_t total_orders{0};
    size_t total_queue_samples{0};

    // L3 Ground Truth Fills
    size_t l3_filled_count{0};
    size_t l3_canceled_count{0};

    // Model Fills
    size_t pessimistic_filled_count{0};
    size_t probabilistic_filled_count{0};
    size_t naive_touch_filled_count{0};

    // False Positives (Model predicted fill, but True L3 was canceled/never filled)
    size_t pessimistic_false_positives{0};
    size_t probabilistic_false_positives{0};
    size_t naive_touch_false_positives{0};

    // Error statistics across all samples
    double pessimistic_bias_shares{0.0}; // Mean signed error (q_model - q_true)
    double pessimistic_mae_shares{0.0};  // Mean absolute error
    double pessimistic_max_err_shares{0.0};

    double probabilistic_bias_shares{0.0};
    double probabilistic_mae_shares{0.0};
    double probabilistic_max_err_shares{0.0};

    // Fill timing error (ns) for orders filled in both L3 and model
    // positive = model was late, negative = model filled too early
    size_t common_fills_pessimistic{0};
    double mean_fill_time_error_pessimistic_ns{0.0};

    size_t common_fills_probabilistic{0};
    double mean_fill_time_error_probabilistic_ns{0.0};

    size_t common_fills_touch{0};
    double mean_fill_time_error_touch_ns{0.0};
};

class QueueEvaluator {
public:
    QueueEvaluator() = default;

    void add_order(const TrackedOrder& order) {
        orders_.push_back(order);
    }

    [[nodiscard]] QueueEvaluationSummary compute_summary() const {
        QueueEvaluationSummary s{};
        s.total_orders = orders_.size();

        double total_err_pess = 0.0;
        double total_abs_pess = 0.0;
        double max_err_pess = 0.0;

        double total_err_prob = 0.0;
        double total_abs_prob = 0.0;
        double max_err_prob = 0.0;

        double sum_fill_dt_pess = 0.0;
        double sum_fill_dt_prob = 0.0;
        double sum_fill_dt_touch = 0.0;

        for (const auto& ord : orders_) {
            s.total_queue_samples += ord.update_count;
            total_err_pess += ord.sum_error_pessimistic;
            total_abs_pess += ord.sum_abs_error_pessimistic;
            max_err_pess = std::max(max_err_pess, ord.max_abs_error_pessimistic);

            total_err_prob += ord.sum_error_probabilistic;
            total_abs_prob += ord.sum_abs_error_probabilistic;
            max_err_prob = std::max(max_err_prob, ord.max_abs_error_probabilistic);

            if (ord.l3_filled) s.l3_filled_count++;
            if (ord.l3_canceled) s.l3_canceled_count++;

            if (ord.pessimistic.filled) {
                s.pessimistic_filled_count++;
                if (!ord.l3_filled) s.pessimistic_false_positives++;
            }

            if (ord.probabilistic.filled) {
                s.probabilistic_filled_count++;
                if (!ord.l3_filled) s.probabilistic_false_positives++;
            }

            if (ord.naive_touch_filled) {
                s.naive_touch_filled_count++;
                if (!ord.l3_filled) s.naive_touch_false_positives++;
            }

            // Fill time comparisons
            if (ord.l3_filled && ord.pessimistic.filled) {
                s.common_fills_pessimistic++;
                sum_fill_dt_pess += static_cast<double>(ord.pessimistic.fill_timestamp_ns) -
                                    static_cast<double>(ord.l3_fill_timestamp_ns);
            }

            if (ord.l3_filled && ord.probabilistic.filled) {
                s.common_fills_probabilistic++;
                sum_fill_dt_prob += static_cast<double>(ord.probabilistic.fill_timestamp_ns) -
                                    static_cast<double>(ord.l3_fill_timestamp_ns);
            }

            if (ord.l3_filled && ord.naive_touch_filled) {
                s.common_fills_touch++;
                sum_fill_dt_touch += static_cast<double>(ord.naive_touch_fill_timestamp_ns) -
                                     static_cast<double>(ord.l3_fill_timestamp_ns);
            }
        }

        if (s.total_queue_samples > 0) {
            s.pessimistic_bias_shares = total_err_pess / static_cast<double>(s.total_queue_samples);
            s.pessimistic_mae_shares = total_abs_pess / static_cast<double>(s.total_queue_samples);
            s.pessimistic_max_err_shares = max_err_pess;

            s.probabilistic_bias_shares = total_err_prob / static_cast<double>(s.total_queue_samples);
            s.probabilistic_mae_shares = total_abs_prob / static_cast<double>(s.total_queue_samples);
            s.probabilistic_max_err_shares = max_err_prob;
        }

        if (s.common_fills_pessimistic > 0) {
            s.mean_fill_time_error_pessimistic_ns = sum_fill_dt_pess / static_cast<double>(s.common_fills_pessimistic);
        }
        if (s.common_fills_probabilistic > 0) {
            s.mean_fill_time_error_probabilistic_ns = sum_fill_dt_prob / static_cast<double>(s.common_fills_probabilistic);
        }
        if (s.common_fills_touch > 0) {
            s.mean_fill_time_error_touch_ns = sum_fill_dt_touch / static_cast<double>(s.common_fills_touch);
        }

        return s;
    }

    [[nodiscard]] const std::vector<TrackedOrder>& orders() const noexcept {
        return orders_;
    }

private:
    std::vector<TrackedOrder> orders_{};
};

} // namespace l3
