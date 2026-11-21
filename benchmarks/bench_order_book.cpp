#include "engine/fast_order_book.hpp"
#include "engine/naive_order_book.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

enum class EventKind : uint8_t {
    Add = 0,
    Cancel = 1,
    Modify = 2
};

struct BenchEvent {
    EventKind kind;
    OrderRequest req;
    OrderId target_id;
    Price new_price;
    Quantity new_qty;
    Timestamp ts;
};

struct BenchSummary {
    std::string name;
    double elapsed_ms = 0.0;
    double ops_per_sec = 0.0;
    double mean_ns = 0.0;
    double p50_ns = 0.0;
    double p90_ns = 0.0;
    double p99_ns = 0.0;
    double p999_ns = 0.0;
    uint64_t trades = 0;
    uint64_t volume = 0;
};

std::vector<BenchEvent> generate_workload(size_t num_events, uint64_t seed) {
    std::vector<BenchEvent> events;
    events.reserve(num_events);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<int> walk_dist(-1, 1);
    std::uniform_int_distribution<int> offset_dist(0, 25);
    std::uniform_int_distribution<Quantity> qty_dist(1, 50);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::vector<OrderId> active_ids;
    active_ids.reserve(num_events / 2);

    Price mid = 50000;
    OrderId next_id = 1;

    for (size_t i = 0; i < num_events; ++i) {
        if ((i & 31) == 0) {
            mid = std::clamp<Price>(mid + walk_dist(rng), 45000, 55000);
        }

        int action = action_dist(rng);
        Timestamp ts = static_cast<Timestamp>(i + 1) * 100;

        if (i < 5000 || action < 52 || active_ids.empty()) {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            OrderType type = OrderType::Limit;
            Price price = 0;

            if (i >= 5000 && action >= 46 && action < 50) {
                type = OrderType::Market;
            } else if (i >= 5000 && action >= 50 && action < 52) {
                type = OrderType::IOC;
                price = (side == Side::Buy) ? (mid + 3) : (mid - 3);
            } else {
                int off = offset_dist(rng);
                bool aggressive = (i >= 5000 && (off == 0));
                if (side == Side::Buy) {
                    price = aggressive ? (mid + 2) : (mid - 1 - off);
                } else {
                    price = aggressive ? (mid - 2) : (mid + 1 + off);
                }
            }

            OrderRequest req{next_id++, price, qty_dist(rng), ts, side, type};
            if (type == OrderType::Limit) {
                active_ids.push_back(req.order_id);
            }
            events.push_back(BenchEvent{EventKind::Add, req, 0, 0, 0, ts});
        } else if (action < 86) {
            size_t pick = rng() % active_ids.size();
            OrderId target = active_ids[pick];
            active_ids[pick] = active_ids.back();
            active_ids.pop_back();
            events.push_back(BenchEvent{EventKind::Cancel, {}, target, 0, 0, ts});
        } else {
            OrderId target = active_ids[rng() % active_ids.size()];
            Price new_price = mid + static_cast<Price>(offset_dist(rng) - 12);
            events.push_back(BenchEvent{EventKind::Modify, {}, target, new_price, qty_dist(rng), ts});
        }
    }

    return events;
}

double percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[idx]);
}

BenchSummary run_naive_bench(const std::vector<BenchEvent>& events, std::vector<uint64_t>& out_latencies) {
    BenchSummary res;
    res.name = "NaiveOrderBook (std::map + std::list)";

    {
        NaiveOrderBook book;
        uint64_t trades = 0;
        uint64_t volume = 0;

        auto t0 = Clock::now();
        for (const auto& ev : events) {
            if (ev.kind == EventKind::Add) {
                auto tr = book.add_order(ev.req);
                trades += tr.size();
                for (const auto& t : tr) volume += t.quantity;
            } else if (ev.kind == EventKind::Cancel) {
                book.cancel_order(ev.target_id);
            } else {
                auto tr = book.modify_order(ev.target_id, ev.new_price, ev.new_qty, ev.ts);
                trades += tr.size();
                for (const auto& t : tr) volume += t.quantity;
            }
        }
        auto t1 = Clock::now();

        double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        res.elapsed_ms = ns / 1e6;
        res.mean_ns = ns / static_cast<double>(events.size());
        res.ops_per_sec = static_cast<double>(events.size()) / (ns / 1e9);
        res.trades = trades;
        res.volume = volume;
    }

    {
        NaiveOrderBook book;
        const size_t warmup = 50000;
        const size_t sample_n = std::min<size_t>(200000, events.size() - warmup);
        out_latencies.clear();
        out_latencies.reserve(sample_n);

        for (size_t i = 0; i < warmup + sample_n; ++i) {
            const auto& ev = events[i];
            if (i < warmup) {
                if (ev.kind == EventKind::Add) book.add_order(ev.req);
                else if (ev.kind == EventKind::Cancel) book.cancel_order(ev.target_id);
                else book.modify_order(ev.target_id, ev.new_price, ev.new_qty, ev.ts);
            } else {
                auto s0 = Clock::now();
                if (ev.kind == EventKind::Add) book.add_order(ev.req);
                else if (ev.kind == EventKind::Cancel) book.cancel_order(ev.target_id);
                else book.modify_order(ev.target_id, ev.new_price, ev.new_qty, ev.ts);
                auto s1 = Clock::now();
                out_latencies.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()));
            }
        }

        auto sorted = out_latencies;
        std::sort(sorted.begin(), sorted.end());
        res.p50_ns = percentile(sorted, 0.50);
        res.p90_ns = percentile(sorted, 0.90);
        res.p99_ns = percentile(sorted, 0.99);
        res.p999_ns = percentile(sorted, 0.999);
    }

    return res;
}

BenchSummary run_fast_bench(const std::vector<BenchEvent>& events, std::vector<uint64_t>& out_latencies) {
    BenchSummary res;
    res.name = "FastOrderBook (Pool + Intrusive + Bitset)";

    {
        FastOrderBook book(40000, 60000, 655360);
        uint64_t trades = 0;
        uint64_t volume = 0;
        auto on_trade = [&](const Trade& t) {
            ++trades;
            volume += t.quantity;
        };

        auto t0 = Clock::now();
        for (const auto& ev : events) {
            if (ev.kind == EventKind::Add) {
                book.add_order_zero_alloc(ev.req, on_trade);
            } else if (ev.kind == EventKind::Cancel) {
                book.cancel_order(ev.target_id);
            } else {
                book.modify_order_zero_alloc(ev.target_id, ev.new_price, ev.new_qty, ev.ts, on_trade);
            }
        }
        auto t1 = Clock::now();

        double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        res.elapsed_ms = ns / 1e6;
        res.mean_ns = ns / static_cast<double>(events.size());
        res.ops_per_sec = static_cast<double>(events.size()) / (ns / 1e9);
        res.trades = trades;
        res.volume = volume;
    }

    {
        FastOrderBook book(40000, 60000, 655360);
        const size_t warmup = 50000;
        const size_t sample_n = std::min<size_t>(200000, events.size() - warmup);
        out_latencies.clear();
        out_latencies.reserve(sample_n);

        uint64_t sink = 0;
        auto on_trade = [&](const Trade& t) { sink += t.quantity; };

        for (size_t i = 0; i < warmup + sample_n; ++i) {
            const auto& ev = events[i];
            if (i < warmup) {
                if (ev.kind == EventKind::Add) book.add_order_zero_alloc(ev.req, on_trade);
                else if (ev.kind == EventKind::Cancel) book.cancel_order(ev.target_id);
                else book.modify_order_zero_alloc(ev.target_id, ev.new_price, ev.new_qty, ev.ts, on_trade);
            } else {
                auto s0 = Clock::now();
                if (ev.kind == EventKind::Add) book.add_order_zero_alloc(ev.req, on_trade);
                else if (ev.kind == EventKind::Cancel) book.cancel_order(ev.target_id);
                else book.modify_order_zero_alloc(ev.target_id, ev.new_price, ev.new_qty, ev.ts, on_trade);
                auto s1 = Clock::now();
                out_latencies.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()));
            }
        }

        volatile uint64_t guard = sink;
        (void)guard;

        auto sorted = out_latencies;
        std::sort(sorted.begin(), sorted.end());
        res.p50_ns = percentile(sorted, 0.50);
        res.p90_ns = percentile(sorted, 0.90);
        res.p99_ns = percentile(sorted, 0.99);
        res.p999_ns = percentile(sorted, 0.999);
    }

    return res;
}

int main() {
    constexpr size_t NUM_EVENTS = 1000000;
    std::cout << "Generating " << NUM_EVENTS << " realistic order book events...\n";
    auto events = generate_workload(NUM_EVENTS, 2026);

    std::vector<uint64_t> naive_lat;
    std::vector<uint64_t> fast_lat;

    auto naive = run_naive_bench(events, naive_lat);
    auto fast  = run_fast_bench(events, fast_lat);

    if (naive.trades != fast.trades || naive.volume != fast.volume) {
        std::cerr << "ERROR: Mismatch between Naive and Fast engine outputs!\n";
        return 1;
    }

    std::cout << "\nVerified identical fills: " << fast.trades
              << " trades, " << fast.volume << " total volume.\n\n";

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::left << std::setw(44) << "Engine"
              << std::right << std::setw(12) << "M ops/s"
              << std::setw(11) << "Mean(ns)"
              << std::setw(10) << "p50(ns)"
              << std::setw(10) << "p90(ns)"
              << std::setw(10) << "p99(ns)"
              << std::setw(11) << "p99.9(ns)" << "\n";
    std::cout << std::string(108, '-') << "\n";

    auto print_row = [](const BenchSummary& s) {
        std::cout << std::left << std::setw(44) << s.name
                  << std::right << std::setw(12) << (s.ops_per_sec / 1e6)
                  << std::setw(11) << s.mean_ns
                  << std::setw(10) << s.p50_ns
                  << std::setw(10) << s.p90_ns
                  << std::setw(10) << s.p99_ns
                  << std::setw(11) << s.p999_ns << "\n";
    };

    print_row(naive);
    print_row(fast);

    std::cout << "\nSpeedup (Throughput): " << (fast.ops_per_sec / naive.ops_per_sec) << "x\n";
    if (fast.p99_ns > 0.0) {
        std::cout << "Speedup (p99 Latency): " << (naive.p99_ns / fast.p99_ns) << "x\n";
    }

    std::ofstream csv("analysis/latency_results.csv");
    if (csv.is_open()) {
        csv << "naive_ns,fast_ns\n";
        for (size_t i = 0; i < naive_lat.size(); ++i) {
            csv << naive_lat[i] << "," << fast_lat[i] << "\n";
        }
        std::cout << "Exported 200,000 latency samples to analysis/latency_results.csv\n";
    }

    return 0;
}
