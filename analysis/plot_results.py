import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

plt.style.use("seaborn-v0_8-darkgrid")
plt.rcParams.update({"font.size": 11, "figure.dpi": 150})

# 1. Matching Engine Latency CDF (Naive vs Fast)
lat = pd.read_csv("analysis/latency_results.csv")
fig, ax = plt.subplots(figsize=(8.5, 5))

for col, label, color in [
    ("fast_ns", "FastOrderBook (Pool + Intrusive + Bitset)", "#1f77b4"),
    ("naive_ns", "NaiveOrderBook (std::map + std::list)", "#d62728"),
]:
    vals = np.sort(lat[col].values)
    cdf = np.arange(1, len(vals) + 1) / len(vals) * 100.0
    ax.plot(vals, cdf, label=label, color=color, linewidth=2.2)

ax.set_xlim(0, 600)
ax.set_ylim(0, 100.5)
ax.axhline(99, color="gray", linestyle="--", linewidth=1, alpha=0.7)
ax.text(510, 96.5, "p99 threshold", color="gray", fontsize=9)
ax.set_title("Order Book Latency CDF (200,000 Warmed Operations)", fontweight="bold")
ax.set_xlabel("Operation Latency (nanoseconds)")
ax.set_ylabel("Cumulative Percentage (%)")
ax.legend(loc="lower right")
plt.tight_layout()
plt.savefig("analysis/engine_latency_cdf.png")
plt.close()

# 2. Latency vs Alpha Decay & Toxic Fills (Avellaneda-Stoikov)
sweep = pd.read_csv("analysis/latency_sweep.csv")
as_df = sweep[sweep["strategy"] == "AvellanedaStoikovMM"]

fig, ax1 = plt.subplots(figsize=(8.5, 5))
color1, color2 = "#1f77b4", "#d62728"

ax1.plot(as_df["latency_us"], as_df["mean_pnl_ticks"], marker="o", color=color1, linewidth=2.2, label="Mean PnL (ticks)")
ax1.axhline(0, color="black", linestyle="--", linewidth=1, alpha=0.6)
ax1.set_xlabel("One-Way Wire Latency (µs)")
ax1.set_ylabel("Mean PnL (ticks)", color=color1)
ax1.tick_params(axis="y", labelcolor=color1)

ax2 = ax1.twinx()
ax2.plot(as_df["latency_us"], as_df["adverse_fill_pct"], marker="s", linestyle="--", color=color2, linewidth=2.0, label="Toxic Sniper Fill %")
ax2.set_ylabel("Adverse / Toxic Fill Rate (%)", color=color2)
ax2.tick_params(axis="y", labelcolor=color2)
ax2.grid(False)

plt.title("Avellaneda-Stoikov: Latency-Induced Alpha Decay (10-Seed Monte Carlo)", fontweight="bold")
fig.tight_layout()
plt.savefig("analysis/latency_alpha_decay.png")
plt.close()

# 3. Single-Session Inventory & PnL Comparison (100 us)
as_ts = pd.read_csv("analysis/sim_as_timeseries.csv")
nv_ts = pd.read_csv("analysis/sim_naive_timeseries.csv")

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 6.5), sharex=True)

ax1.plot(as_ts["time_sec"], as_ts["mtm_pnl_ticks"], label="Avellaneda-Stoikov MM", color="#1f77b4", linewidth=2)
ax1.plot(nv_ts["time_sec"], nv_ts["mtm_pnl_ticks"], label="Naive Symmetric MM", color="#ff7f0e", linewidth=1.8, alpha=0.85)
ax1.set_ylabel("Mark-to-Market PnL (ticks)")
ax1.set_title("60-Second Microstructure Session Comparison (Wire Latency = 100 µs)", fontweight="bold")
ax1.legend(loc="upper left")

ax2.plot(as_ts["time_sec"], as_ts["inventory"], label="Avellaneda-Stoikov (InvStd: 13.6)", color="#1f77b4", linewidth=2)
ax2.plot(nv_ts["time_sec"], nv_ts["inventory"], label="Naive Symmetric (InvStd: 93.6)", color="#ff7f0e", linewidth=1.8, alpha=0.85)
ax2.axhline(0, color="black", linestyle="--", linewidth=0.8, alpha=0.6)
ax2.set_xlabel("Simulation Time (seconds)")
ax2.set_ylabel("Net Inventory (shares)")
ax2.legend(loc="lower left")

plt.tight_layout()
plt.savefig("analysis/strategy_timeseries.png")
plt.close()

print("Generated 3 PNG charts in analysis/.")