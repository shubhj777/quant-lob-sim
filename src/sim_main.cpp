#include "sim/microstructure_sim.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace lob;

int main() {
    MicrostructureSimulator sim;

    {
        SimConfig cfg;
        cfg.seed = 2026;
        cfg.latency_ns = 100'000ULL;
        cfg.record_timeseries = true;

        AvellanedaStoikovMM as_mm;
        cfg.timeseries_path = "analysis/sim_as_timeseries.csv";
        auto r_as = sim.run(as_mm, cfg);

        NaiveSymmetricMM naive_mm;
        cfg.timeseries_path = "analysis/sim_naive_timeseries.csv";
        auto r_naive = sim.run(naive_mm, cfg);

        std::cout << "=== Single-Session Head-to-Head (Wire Latency = 100 us, 60s Horizon) ===\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Avellaneda-Stoikov -> PnL: " << r_as.final_pnl_ticks
                  << " ticks | Sharpe: " << r_as.sharpe_ratio
                  << " | MaxDD: " << r_as.max_drawdown_ticks
                  << " | InvStd: " << r_as.inv_std
                  << " | Max|Inv|: " << r_as.max_abs_inv << "\n";
        std::cout << "Naive Symmetric MM -> PnL: " << r_naive.final_pnl_ticks
                  << " ticks | Sharpe: " << r_naive.sharpe_ratio
                  << " | MaxDD: " << r_naive.max_drawdown_ticks
                  << " | InvStd: " << r_naive.inv_std
                  << " | Max|Inv|: " << r_naive.max_abs_inv << "\n\n";
    }

    const std::vector<Timestamp> latencies_us = {0, 25, 50, 100, 200, 400, 800, 1500};
    const int num_seeds = 10;

    std::ofstream sweep_csv("analysis/latency_sweep.csv");
    sweep_csv << "strategy,latency_us,mean_pnl_ticks,mean_sharpe,mean_max_dd,mean_inv_std,adverse_fill_pct\n";

    std::cout << "=== Monte Carlo Latency & Adverse Selection Sweep (" << num_seeds << " seeds per point) ===\n";
    std::cout << std::left << std::setw(22) << "Strategy"
              << std::right << std::setw(12) << "Latency(us)"
              << std::setw(14) << "MeanPnL(tks)"
              << std::setw(11) << "Sharpe"
              << std::setw(12) << "MaxDD(tks)"
              << std::setw(11) << "InvStd"
              << std::setw(13) << "ToxicFill%" << "\n";
    std::cout << std::string(95, '-') << "\n";

    auto run_sweep = [&](IMarketMaker& mm) {
        for (Timestamp lat_us : latencies_us) {
            double sum_pnl = 0.0, sum_sharpe = 0.0, sum_dd = 0.0, sum_inv_std = 0.0, sum_toxic = 0.0;

            for (int s = 0; s < num_seeds; ++s) {
                SimConfig cfg;
                cfg.seed = 1000 + static_cast<uint64_t>(s);
                cfg.latency_ns = lat_us * 1000ULL;
                auto r = sim.run(mm, cfg);

                sum_pnl += r.final_pnl_ticks;
                sum_sharpe += r.sharpe_ratio;
                sum_dd += r.max_drawdown_ticks;
                sum_inv_std += r.inv_std;
                double toxic_pct = (r.total_fills > 0)
                    ? (100.0 * static_cast<double>(r.adverse_fills) / static_cast<double>(r.total_fills))
                    : 0.0;
                sum_toxic += toxic_pct;
            }

            double d = static_cast<double>(num_seeds);
            std::cout << std::left << std::setw(22) << mm.name()
                      << std::right << std::setw(12) << lat_us
                      << std::setw(14) << (sum_pnl / d)
                      << std::setw(11) << (sum_sharpe / d)
                      << std::setw(12) << (sum_dd / d)
                      << std::setw(11) << (sum_inv_std / d)
                      << std::setw(12) << (sum_toxic / d) << "%\n";

            sweep_csv << mm.name() << ","
                      << lat_us << ","
                      << (sum_pnl / d) << ","
                      << (sum_sharpe / d) << ","
                      << (sum_dd / d) << ","
                      << (sum_inv_std / d) << ","
                      << (sum_toxic / d) << "\n";
        }
    };

    AvellanedaStoikovMM as_mm;
    NaiveSymmetricMM naive_mm;
    run_sweep(as_mm);
    std::cout << std::string(95, '-') << "\n";
    run_sweep(naive_mm);

    std::cout << "\nExported timeseries and latency sweep CSVs to analysis/.\n";
    return 0;
}