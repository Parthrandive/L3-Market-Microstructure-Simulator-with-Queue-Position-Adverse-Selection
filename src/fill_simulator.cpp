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
    std::cout << "Strategy: Passive Buy 100 shares @ Best Bid when OBI > 0.0, 5s TTL, 250 ms sampling rate\n";
    std::cout << "Definition: Naive touch fill assumes immediate fill upon any trade at or through limit price,\n";
    std::cout << "            disregarding queue priority, resting depth ahead, and adverse selection.\n";
    std::cout << "====================================================================================================================================\n\n";

    // 1. Run baseline engine (delta_t = 0)
    std::cout << "Running replay for Delta t = 0 us (Model 1: Naive Touch & Model 2: Queue-Aware L3) ... " << std::flush;
    auto t0 = std::chrono::steady_clock::now();
    SimulationConfig cfg_0{ .delta_t_ns = 0, .ttl_ns = 5'000'000'000ULL, .obi_threshold = 0.0, .sampling_interval_ns = 250'000'000ULL };
    FillSimulationEngine engine_0(filepath, cfg_0);
    std::vector<SimOrder> orders_0 = engine_0.run();
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "Done (" << std::fixed << std::setprecision(2) << std::chrono::duration<double>(t1 - t0).count() << " s, "
              << orders_0.size() << " orders submitted)\n";

    // 2. Run latency engine delta_t = 50 us (Model 3a: Metro fiber transit)
    std::cout << "Running replay for Delta t = 50 us (Model 3a: Queue-Aware L3 + 50 us latency) ... " << std::flush;
    t0 = std::chrono::steady_clock::now();
    SimulationConfig cfg_50{ .delta_t_ns = 50'000ULL, .ttl_ns = 5'000'000'000ULL, .obi_threshold = 0.0, .sampling_interval_ns = 250'000'000ULL };
    FillSimulationEngine engine_50(filepath, cfg_50);
    std::vector<SimOrder> orders_50 = engine_50.run();
    t1 = std::chrono::steady_clock::now();
    std::cout << "Done (" << std::fixed << std::setprecision(2) << std::chrono::duration<double>(t1 - t0).count() << " s, "
              << orders_50.size() << " orders submitted)\n";

    // 3. Run latency engine delta_t = 250 us (Model 3b: Inter-exchange transit)
    std::cout << "Running replay for Delta t = 250 us (Model 3b: Queue-Aware L3 + 250 us latency) ... " << std::flush;
    t0 = std::chrono::steady_clock::now();
    SimulationConfig cfg_250{ .delta_t_ns = 250'000ULL, .ttl_ns = 5'000'000'000ULL, .obi_threshold = 0.0, .sampling_interval_ns = 250'000'000ULL };
    FillSimulationEngine engine_250(filepath, cfg_250);
    std::vector<SimOrder> orders_250 = engine_250.run();
    t1 = std::chrono::steady_clock::now();
    std::cout << "Done (" << std::fixed << std::setprecision(2) << std::chrono::duration<double>(t1 - t0).count() << " s, "
              << orders_250.size() << " orders submitted)\n\n";

    // Evaluate models
    // First obtain true L3 fills at delta_t = 0 as benchmark reference
    size_t l3_ref_fills = 0;
    for (const auto& ord : orders_0) {
        if (ord.l3_filled) l3_ref_fills++;
    }

    FillModelSummary m1_touch = evaluate_model_performance(orders_0, true, "Model 1: Naive Touch Fill", 0, l3_ref_fills);
    FillModelSummary m2_l3_0 = evaluate_model_performance(orders_0, false, "Model 2: Queue L3 (0 us)", 0, l3_ref_fills);
    FillModelSummary m3_l3_50 = evaluate_model_performance(orders_50, false, "Model 3a: Queue L3 (50 us)", 50'000ULL, l3_ref_fills);
    FillModelSummary m3_l3_250 = evaluate_model_performance(orders_250, false, "Model 3b: Queue L3 (250 us)", 250'000ULL, l3_ref_fills);

    std::vector<FillModelSummary> models = { m1_touch, m2_l3_0, m3_l3_50, m3_l3_250 };

    // Print Comparative Table 1: Fill Rates, Overstatement, and Lead Times
    std::cout << "====================================================================================================================================\n";
    std::cout << "TABLE 1: EXECUTION COMPARISON & TOUCH OVERSTATEMENT (95% Time-Block Bootstrap CI)\n";
    std::cout << "====================================================================================================================================\n";
    std::cout << std::left << std::setw(30) << "Model"
              << std::right << std::setw(10) << "Orders"
              << std::setw(8) << "Fills"
              << std::setw(24) << "Fill Rate (95% CI)"
              << std::setw(24) << "Overstatement Ratio"
              << std::setw(24) << "Mean Lead Time (ms)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& m : models) {
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

    for (const auto& m : models) {
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

    // Print Comparative Table 3: Strategy Realized PnL Across Fill Models
    std::cout << "====================================================================================================================================\n";
    std::cout << "TABLE 3: STRATEGY REALIZED PnL ACROSS FILL MODELS (100 shares/fill, 95% Bootstrap CI for 30s PnL)\n";
    std::cout << "====================================================================================================================================\n";
    std::cout << std::left << std::setw(30) << "Model"
              << std::right << std::setw(18) << "Total PnL 5s ($)"
              << std::setw(18) << "PnL/Order 5s ($)"
              << std::setw(19) << "Total PnL 30s ($)"
              << std::setw(18) << "PnL/Order 30s ($)"
              << std::setw(28) << "Total PnL 30s (95% CI)\n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& m : models) {
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
    std::cout << "### NASDAQ ITCH 5.0 LIMIT ORDER BOOK MICROSTRUCTURE SIMULATOR: STAGE 4 RESULTS\n";
    std::cout << "### Evaluating Naive Touch Fill Overstatement, Queue Depletion Priority, and Adverse Selection\n";
    std::cout << "####################################################################################################################################\n";

    // Day 1: Dec 30, 2019
    print_stage4_results_for_date(day1_path, "2019-12-30", "Holiday Lull Low-Volatility Session");

    // Day 2: Oct 30, 2019
    print_stage4_results_for_date(day2_path, "2019-10-30", "FOMC Rate Decision Volatile Session");

    return 0;
}
