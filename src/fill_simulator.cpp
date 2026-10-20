#include "orderbook/fill_simulator.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

using namespace l3;

void print_stage4_results_for_date(const std::string& filepath, const std::string& date_label, const std::string& regime_label) {
    std::cout << "\n====================================================================================================================================\n";
    std::cout << "STAGE 4: FILL SIMULATOR & ADVERSE SELECTION ANALYSIS\n";
    std::cout << "Date: " << date_label << " (" << regime_label << ")\n";
    std::cout << "Fixture: " << filepath << " (Regular Hours 09:30 - 16:00 ET Strictly Filtered)\n";
    std::cout << "Primary Baseline: Passive Buy 100 sh @ Best Bid when OBI > 0.0, 5s TTL, 250 ms sampling interval\n";
    std::cout << "Definition: Naive touch fill assumes immediate fill upon any trade at or through limit price,\n";
    std::cout << "            disregarding queue priority, resting depth ahead, and adverse selection.\n";
    std::cout << "====================================================================================================================================\n\n";

    // 1. Full wire latency sweep: Delta t in {0, 50, 100, 250, 500 us}
    std::vector<uint64_t> latencies_ns = { 0, 50'000ULL, 100'000ULL, 250'000ULL, 500'000ULL };
    std::vector<std::vector<SimOrder>> sweep_orders;
    sweep_orders.reserve(latencies_ns.size());

    for (uint64_t dt : latencies_ns) {
        std::cout << "Running replay for Delta t = " << (dt / 1000) << " us ... " << std::flush;
        auto t0 = std::chrono::steady_clock::now();
        SimulationConfig cfg{ .delta_t_ns = dt, .ttl_ns = 5'000'000'000ULL, .obi_threshold = 0.0, .sampling_interval_ns = 250'000'000ULL, .order_shares = 100 };
        FillSimulationEngine engine(filepath, cfg);
        std::vector<SimOrder> ords = engine.run();
        auto t1 = std::chrono::steady_clock::now();
        std::cout << "Done (" << std::fixed << std::setprecision(2) << std::chrono::duration<double>(t1 - t0).count() << " s, "
                  << ords.size() << " orders submitted)\n";
        sweep_orders.push_back(std::move(ords));
    }

    // Benchmark baseline fills for overstatement ratio
    size_t l3_ref_fills = 0;
    for (const auto& ord : sweep_orders[0]) {
        if (ord.l3_filled) l3_ref_fills++;
    }

    FillModelSummary m1_touch = evaluate_model_performance(sweep_orders[0], true, "Model 1: Naive Touch Fill", 0, l3_ref_fills);
    FillModelSummary m2_l3_0 = evaluate_model_performance(sweep_orders[0], false, "Model 2: Queue L3 (0 us)", 0, l3_ref_fills);
    FillModelSummary m3_l3_50 = evaluate_model_performance(sweep_orders[1], false, "Model 3a: Queue L3 (50 us)", 50'000ULL, l3_ref_fills);
    FillModelSummary m3_l3_100 = evaluate_model_performance(sweep_orders[2], false, "Model 3b: Queue L3 (100 us)", 100'000ULL, l3_ref_fills);
    FillModelSummary m3_l3_250 = evaluate_model_performance(sweep_orders[3], false, "Model 3c: Queue L3 (250 us)", 250'000ULL, l3_ref_fills);
    FillModelSummary m3_l3_500 = evaluate_model_performance(sweep_orders[4], false, "Model 3d: Queue L3 (500 us)", 500'000ULL, l3_ref_fills);

    std::vector<FillModelSummary> primary_models = { m1_touch, m2_l3_0, m3_l3_50, m3_l3_100, m3_l3_250, m3_l3_500 };

    // Print Comparative Table 1: Fill Rates, Overstatement, and Lead Times
    std::cout << "\n====================================================================================================================================\n";
    std::cout << "TABLE 1: EXECUTION COMPARISON & TOUCH OVERSTATEMENT (95% Time-Block Bootstrap CI)\n";
    std::cout << "====================================================================================================================================\n";
    std::cout << std::left << std::setw(30) << "Model"
              << std::right << std::setw(10) << "Orders"
              << std::setw(8) << "Fills"
              << std::setw(24) << "Fill Rate (95% CI)"
              << std::setw(24) << "Overstatement Ratio"
              << std::setw(24) << "Mean Lead Time (ms)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& m : primary_models) {
        std::ostringstream ss_rate, ss_over, ss_lead;
        ss_rate << std::fixed << std::setprecision(2) << m.fill_rate_ci.point_est << "% ["
                << m.fill_rate_ci.ci_lo << "%-" << m.fill_rate_ci.ci_hi << "%]";
        
        if (m.model_name.find("Naive Touch") != std::string::npos) {
            ss_over << std::fixed << std::setprecision(2) << m.overstatement_ratio_ci.point_est << "x ["
                    << m.overstatement_ratio_ci.ci_lo << "x-" << m.overstatement_ratio_ci.ci_hi << "x]";
            ss_lead << std::fixed << std::setprecision(1) << m.mean_lead_time_ms_ci.point_est << " ms ["
                    << m.mean_lead_time_ms_ci.ci_lo << "-" << m.mean_lead_time_ms_ci.ci_hi << "]";
        } else {
            ss_over << "1.00x (Baseline)";
            ss_lead << "0.0 ms (Reference)";
        }

        std::cout << std::left << std::setw(30) << m.model_name
                  << std::right << std::setw(10) << m.total_orders
                  << std::setw(8) << m.total_fills
                  << std::setw(24) << ss_rate.str()
                  << std::setw(24) << ss_over.str()
                  << std::setw(24) << ss_lead.str() << "\n";
    }
    std::cout << "====================================================================================================================================\n\n";

    // Print Comparative Table 2: Adverse Selection & Post-Fill Mid-Price Markouts
    std::cout << "====================================================================================================================================\n";
    std::cout << "TABLE 2: POST-FILL MID-PRICE MARKOUT & ADVERSE SELECTION DRIFT (in Basis Points)\n";
    std::cout << "Note: Markout = (Mid_tau - FillPrice)/FillPrice * 10,000 bps = HalfSpread + MidDrift_tau\n";
    std::cout << "====================================================================================================================================\n";
    std::cout << std::left << std::setw(30) << "Model"
              << std::right << std::setw(16) << "Half-Spread (bps)"
              << std::setw(20) << "Markout 1s (bps)"
              << std::setw(20) << "Markout 5s (bps)"
              << std::setw(20) << "Markout 30s (bps)"
              << std::setw(20) << "30s Drift (bps)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& m : primary_models) {
        std::ostringstream ss_m1, ss_m5, ss_m30;
        ss_m1 << std::fixed << std::setprecision(2) << m.markout_1s_ci.point_est << " ["
              << m.markout_1s_ci.ci_lo << ".." << m.markout_1s_ci.ci_hi << "]";
        ss_m5 << std::fixed << std::setprecision(2) << m.markout_5s_ci.point_est << " ["
              << m.markout_5s_ci.ci_lo << ".." << m.markout_5s_ci.ci_hi << "]";
        ss_m30 << std::fixed << std::setprecision(2) << m.markout_30s_ci.point_est << " ["
               << m.markout_30s_ci.ci_lo << ".." << m.markout_30s_ci.ci_hi << "]";

        std::cout << std::left << std::setw(30) << m.model_name
                  << std::right << std::setw(16) << std::fixed << std::setprecision(2) << m.avg_half_spread_captured_bps
                  << std::setw(20) << ss_m1.str()
                  << std::setw(20) << ss_m5.str()
                  << std::setw(20) << ss_m30.str()
                  << std::setw(20) << std::fixed << std::setprecision(2) << m.mid_drift_30s_bps << "\n";
    }
    std::cout << "====================================================================================================================================\n\n";

    // Print Comparative Table 3: Strategy Mark-to-Mid Markout PnL Across Fill Models
    std::cout << "====================================================================================================================================\n";
    std::cout << "TABLE 3: MARK-TO-MID MARKOUT PnL, BEFORE FEES AND EXIT COSTS (100 sh/fill, 95% Bootstrap CI)\n";
    std::cout << "Disclosure: Marks filled inventory to mid price at horizon. Does not model liquidation or fee schedules.\n";
    std::cout << "====================================================================================================================================\n";
    std::cout << std::left << std::setw(30) << "Model"
              << std::right << std::setw(18) << "Total PnL 5s ($)"
              << std::setw(18) << "PnL/Order 5s ($)"
              << std::setw(19) << "Total PnL 30s ($)"
              << std::setw(18) << "PnL/Order 30s ($)"
              << std::setw(28) << "Total PnL 30s (95% CI)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& m : primary_models) {
        std::ostringstream ss_pnl_ci;
        ss_pnl_ci << "$" << std::fixed << std::setprecision(2) << m.pnl_30s_usd_ci.point_est << " [$"
                  << m.pnl_30s_usd_ci.ci_lo << "..$" << m.pnl_30s_usd_ci.ci_hi << "]";

        std::cout << std::left << std::setw(30) << m.model_name
                  << std::right << std::setw(18) << std::fixed << std::setprecision(2) << m.total_pnl_5s_usd
                  << std::setw(18) << std::fixed << std::setprecision(4) << m.pnl_per_order_5s_usd
                  << std::setw(19) << std::fixed << std::setprecision(2) << m.total_pnl_30s_usd
                  << std::setw(18) << std::fixed << std::setprecision(4) << m.pnl_per_order_30s_usd
                  << std::setw(28) << ss_pnl_ci.str() << "\n";
    }
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";
    if (m1_touch.pnl_ratio_5s_ci.point_est > 0.0) {
        std::cout << "Touch vs Queue L3 5s PnL Ratio: " << std::fixed << std::setprecision(2)
                  << (m1_touch.total_pnl_5s_usd / m2_l3_0.total_pnl_5s_usd) << "x [95% CI: "
                  << m1_touch.pnl_ratio_5s_ci.ci_lo << "x - " << m1_touch.pnl_ratio_5s_ci.ci_hi << "x]\n";
    }
    std::cout << "====================================================================================================================================\n\n";

    // 2. Full Parameter Sensitivity Grid (18 combinations)
    std::cout << "====================================================================================================================================\n";
    std::cout << "TABLE 4: COMPREHENSIVE PARAMETER SENSITIVITY GRID (Full Multiple-Testing Space Disclosed)\n";
    std::cout << "Combinations: Sizes in {100, 500 sh} x TTLs in {1s, 5s, 30s} x OBI Thresholds in {Unconditional (-1.0), >0.0, >0.20}\n";
    std::cout << "====================================================================================================================================\n";
    std::cout << std::left << std::setw(10) << "Size (sh)"
              << std::setw(10) << "TTL"
              << std::setw(16) << "OBI Threshold"
              << std::right << std::setw(10) << "Orders"
              << std::setw(14) << "Touch Rate"
              << std::setw(14) << "True L3 Rate"
              << std::setw(16) << "Overstatement"
              << std::setw(16) << "Markout 5s (bps)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";

    std::vector<uint32_t> test_sizes = { 100, 500 };
    std::vector<uint64_t> test_ttls = { 1'000'000'000ULL, 5'000'000'000ULL, 30'000'000'000ULL };
    std::vector<std::pair<double, std::string>> test_obis = {
        { -1.0, "Unconditional" },
        { 0.0, "OBI > 0.0" },
        { 0.20, "OBI > 0.20" }
    };

    for (uint32_t sz : test_sizes) {
        for (uint64_t ttl : test_ttls) {
            for (const auto& [obi_thr, obi_label] : test_obis) {
                SimulationConfig s_cfg{
                    .delta_t_ns = 0,
                    .ttl_ns = ttl,
                    .obi_threshold = obi_thr,
                    .sampling_interval_ns = 250'000'000ULL,
                    .order_shares = sz
                };
                FillSimulationEngine s_engine(filepath, s_cfg);
                std::vector<SimOrder> s_ords = s_engine.run();

                size_t s_touch = 0, s_l3 = 0;
                double s_m5_sum = 0.0;
                for (const auto& o : s_ords) {
                    if (o.touch_filled) s_touch++;
                    if (o.l3_filled) {
                        s_l3++;
                        s_m5_sum += o.l3_markout_5s_bps;
                    }
                }

                double touch_rate = s_ords.empty() ? 0.0 : (100.0 * s_touch / s_ords.size());
                double l3_rate = s_ords.empty() ? 0.0 : (100.0 * s_l3 / s_ords.size());
                double overstate = s_l3 > 0 ? (static_cast<double>(s_touch) / s_l3) : 0.0;
                double avg_m5 = s_l3 > 0 ? (s_m5_sum / s_l3) : 0.0;

                std::string ttl_str = std::to_string(ttl / 1'000'000'000ULL) + "s";

                std::cout << std::left << std::setw(10) << sz
                          << std::setw(10) << ttl_str
                          << std::setw(16) << obi_label
                          << std::right << std::setw(10) << s_ords.size()
                          << std::setw(13) << std::fixed << std::setprecision(2) << touch_rate << "%"
                          << std::setw(13) << std::fixed << std::setprecision(2) << l3_rate << "%"
                          << std::setw(15) << std::fixed << std::setprecision(2) << overstate << "x"
                          << std::setw(16) << std::fixed << std::setprecision(2) << avg_m5 << "\n";
            }
        }
    }
    std::cout << "====================================================================================================================================\n\n";
}

int main(int argc, char* argv[]) {
    std::string day1_path = "data/fixture_3symbols_full.bin.gz";
    std::string day2_path = "data/fixture_3symbols_10302019.bin.gz";

    if (argc > 1) {
        day1_path = argv[1];
    }
    if (argc > 2) {
        day2_path = argv[2];
    }

    std::cout << "\n####################################################################################################################################\n";
    std::cout << "### NASDAQ ITCH 5.0 LIMIT ORDER BOOK MICROSTRUCTURE SIMULATOR: STAGE 4 COMPLETE RIGOROUS SUITE\n";
    std::cout << "### Evaluating Naive Touch Fill Overstatement, Queue Depletion Priority, and Adverse Selection\n";
    std::cout << "####################################################################################################################################\n";

    // Day 1: Dec 30, 2019
    print_stage4_results_for_date(day1_path, "2019-12-30", "Holiday Lull Low-Volatility Session");

    // Day 2: Oct 30, 2019
    print_stage4_results_for_date(day2_path, "2019-10-30", "FOMC Rate Decision Volatile Session");

    return 0;
}
