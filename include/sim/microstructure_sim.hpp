#pragma once

#include "engine/fast_order_book.hpp"
#include "strategy/market_maker.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <queue>
#include <random>
#include <string>
#include <vector>

namespace lob {

struct SimConfig {
    uint64_t seed = 42;
    Timestamp duration_ns = 60'000'000'000ULL;
    Timestamp latency_ns = 100'000ULL;
    Price initial_price = 50000;
    double lp_rate_per_sec = 600.0;
    double retail_rate_per_sec = 320.0;
    double jump_rate_per_sec = 45.0;
    double mean_sniper_delay_ns = 250'000.0;
    bool record_timeseries = false;
    std::string timeseries_path = "";
};

struct SimResult {
    std::string strategy_name;
    double latency_us = 0.0;
    double final_pnl_ticks = 0.0;
    double max_drawdown_ticks = 0.0;
    double sharpe_ratio = 0.0;
    double inv_std = 0.0;
    int32_t max_abs_inv = 0;
    int32_t final_inv = 0;
    uint64_t total_fills = 0;
    uint64_t adverse_fills = 0;
    uint64_t traded_volume = 0;
};

class MicrostructureSimulator {
private:
    static constexpr OrderId MM_ORDER_BIT = (1ULL << 60);
    static constexpr Price MIN_BOOK_PRICE = 40000;
    static constexpr Price MAX_BOOK_PRICE = 60000;

    enum class EventType : uint8_t {
        BackgroundLP = 0,
        RetailMarketOrder = 1,
        FairPriceJump = 2,
        InformedSniper = 3,
        MarketDataToStrategy = 4,
        FillToStrategy = 5,
        StrategyAdd = 6,
        StrategyCancel = 7,
        SampleMetrics = 8
    };

    struct SimEvent {
        Timestamp ts = 0;
        uint64_t  seq = 0;
        EventType type = EventType::BackgroundLP;
        OrderId   order_id = 0;
        Price     price = 0;
        Quantity  qty = 0;
        Side      side = Side::Buy;
        double    ref_price = 0.0;
        Price     snap_bid = INVALID_PRICE;
        Price     snap_ask = INVALID_PRICE;
        Quantity  snap_bid_vol = 0;
        Quantity  snap_ask_vol = 0;

        bool operator>(const SimEvent& other) const {
            if (ts != other.ts) return ts > other.ts;
            return seq > other.seq;
        }
    };

    static Price clamp_price(Price p) {
        return std::clamp<Price>(p, MIN_BOOK_PRICE + 500, MAX_BOOK_PRICE - 500);
    }

    static bool is_mm_order(OrderId id) {
        return (id & MM_ORDER_BIT) != 0;
    }

public:
    SimResult run(IMarketMaker& strategy, const SimConfig& cfg) {
        strategy.reset();

        FastOrderBook book(MIN_BOOK_PRICE, MAX_BOOK_PRICE, 262144);
        std::mt19937_64 rng(cfg.seed);

        std::exponential_distribution<double> lp_iat(cfg.lp_rate_per_sec / 1e9);
        std::exponential_distribution<double> retail_iat(cfg.retail_rate_per_sec / 1e9);
        std::exponential_distribution<double> jump_iat(cfg.jump_rate_per_sec / 1e9);
        std::exponential_distribution<double> sniper_delay_dist(1.0 / cfg.mean_sniper_delay_ns);

        std::uniform_int_distribution<int> side_dist(0, 1);
        std::uniform_int_distribution<int> lp_offset_dist(2, 10);
        std::uniform_int_distribution<Quantity> lp_qty_dist(5, 20);
        std::uniform_int_distribution<Quantity> retail_qty_dist(1, 5);
        std::uniform_int_distribution<int> jump_mag_dist(2, 4);

        std::priority_queue<SimEvent, std::vector<SimEvent>, std::greater<SimEvent>> pq;
        uint64_t next_seq = 1;
        OrderId next_bg_id = 1;
        OrderId next_mm_seq = 1;

        auto push_event = [&](SimEvent ev) {
            ev.seq = next_seq++;
            pq.push(ev);
        };

        double fair_price = static_cast<double>(cfg.initial_price);
        std::deque<OrderId> bg_orders;

        for (int off = 2; off <= 10; ++off) {
            for (int k = 0; k < 2; ++k) {
                OrderId b_id = next_bg_id++;
                OrderId a_id = next_bg_id++;
                book.add_order_zero_alloc({b_id, cfg.initial_price - off, 15, 0, Side::Buy, OrderType::Limit}, [](const Trade&) {});
                book.add_order_zero_alloc({a_id, cfg.initial_price + off, 15, 0, Side::Sell, OrderType::Limit}, [](const Trade&) {});
                bg_orders.push_back(b_id);
                bg_orders.push_back(a_id);
            }
        }

        double true_cash = 0.0;
        int32_t true_inv = 0;
        uint64_t total_fills = 0;
        uint64_t adverse_fills = 0;
        uint64_t traded_volume = 0;

        int32_t strat_inv = 0;
        OrderId active_bid_id = 0;
        OrderId active_ask_id = 0;
        Price   quoted_bid_price = INVALID_PRICE;
        Price   quoted_ask_price = INVALID_PRICE;
        bool    is_sniper_context = false;

        auto publish_md = [&](Timestamp now) {
            SimEvent md;
            md.ts = now + cfg.latency_ns;
            md.type = EventType::MarketDataToStrategy;
            md.ref_price = fair_price;
            md.snap_bid = book.best_bid();
            md.snap_ask = book.best_ask();
            md.snap_bid_vol = (md.snap_bid == INVALID_PRICE) ? 0 : book.bid_volume_at(md.snap_bid);
            md.snap_ask_vol = (md.snap_ask == INVALID_PRICE) ? 0 : book.ask_volume_at(md.snap_ask);
            push_event(md);
        };

        auto handle_trade = [&](const Trade& t) {
            bool maker_mm = is_mm_order(t.maker_order_id);
            bool taker_mm = is_mm_order(t.taker_order_id);
            if (!maker_mm && !taker_mm) return;

            Side mm_side = maker_mm
                ? (t.taker_side == Side::Buy ? Side::Sell : Side::Buy)
                : t.taker_side;

            double notional = static_cast<double>(t.price) * static_cast<double>(t.quantity);
            if (mm_side == Side::Buy) {
                true_cash -= notional;
                true_inv += static_cast<int32_t>(t.quantity);
            } else {
                true_cash += notional;
                true_inv -= static_cast<int32_t>(t.quantity);
            }

            ++total_fills;
            traded_volume += t.quantity;
            if (is_sniper_context && maker_mm) {
                ++adverse_fills;
            }

            SimEvent fill_ev;
            fill_ev.ts = t.timestamp + cfg.latency_ns;
            fill_ev.type = EventType::FillToStrategy;
            fill_ev.order_id = maker_mm ? t.maker_order_id : t.taker_order_id;
            fill_ev.side = mm_side;
            fill_ev.price = t.price;
            fill_ev.qty = t.quantity;
            push_event(fill_ev);
        };

        auto apply_strategy_quotes = [&](Timestamp now, const MarketSnapshot& snap) {
            double time_frac = static_cast<double>(now) / static_cast<double>(cfg.duration_ns);
            QuoteDecision q = strategy.on_market_update(snap, strat_inv, time_frac);

            if (q.quote_bid) {
                Price target = clamp_price(q.bid_price);
                if (target != quoted_bid_price || active_bid_id == 0) {
                    if (active_bid_id != 0) {
                        SimEvent cancel_ev;
                        cancel_ev.ts = now + cfg.latency_ns;
                        cancel_ev.type = EventType::StrategyCancel;
                        cancel_ev.order_id = active_bid_id;
                        push_event(cancel_ev);
                    }
                    active_bid_id = MM_ORDER_BIT | (next_mm_seq++);
                    quoted_bid_price = target;
                    SimEvent add_ev;
                    add_ev.ts = now + cfg.latency_ns;
                    add_ev.type = EventType::StrategyAdd;
                    add_ev.order_id = active_bid_id;
                    add_ev.price = target;
                    add_ev.qty = q.bid_qty;
                    add_ev.side = Side::Buy;
                    push_event(add_ev);
                }
            } else if (active_bid_id != 0) {
                SimEvent cancel_ev;
                cancel_ev.ts = now + cfg.latency_ns;
                cancel_ev.type = EventType::StrategyCancel;
                cancel_ev.order_id = active_bid_id;
                push_event(cancel_ev);
                active_bid_id = 0;
                quoted_bid_price = INVALID_PRICE;
            }

            if (q.quote_ask) {
                Price target = clamp_price(q.ask_price);
                if (target != quoted_ask_price || active_ask_id == 0) {
                    if (active_ask_id != 0) {
                        SimEvent cancel_ev;
                        cancel_ev.ts = now + cfg.latency_ns;
                        cancel_ev.type = EventType::StrategyCancel;
                        cancel_ev.order_id = active_ask_id;
                        push_event(cancel_ev);
                    }
                    active_ask_id = MM_ORDER_BIT | (next_mm_seq++);
                    quoted_ask_price = target;
                    SimEvent add_ev;
                    add_ev.ts = now + cfg.latency_ns;
                    add_ev.type = EventType::StrategyAdd;
                    add_ev.order_id = active_ask_id;
                    add_ev.price = target;
                    add_ev.qty = q.ask_qty;
                    add_ev.side = Side::Sell;
                    push_event(add_ev);
                }
            } else if (active_ask_id != 0) {
                SimEvent cancel_ev;
                cancel_ev.ts = now + cfg.latency_ns;
                cancel_ev.type = EventType::StrategyCancel;
                cancel_ev.order_id = active_ask_id;
                push_event(cancel_ev);
                active_ask_id = 0;
                quoted_ask_price = INVALID_PRICE;
            }
        };

        push_event({static_cast<Timestamp>(lp_iat(rng)), 0, EventType::BackgroundLP});
        push_event({static_cast<Timestamp>(retail_iat(rng)), 0, EventType::RetailMarketOrder});
        push_event({static_cast<Timestamp>(jump_iat(rng)), 0, EventType::FairPriceJump});
        publish_md(0);

        constexpr Timestamp SAMPLE_STEP_NS = 100'000'000ULL;
        push_event({SAMPLE_STEP_NS, 0, EventType::SampleMetrics});

        std::ofstream ts_csv;
        if (cfg.record_timeseries && !cfg.timeseries_path.empty()) {
            ts_csv.open(cfg.timeseries_path);
            ts_csv << "time_sec,fair_price,best_bid,best_ask,inventory,mtm_pnl_ticks\n";
        }

        double peak_pnl = 0.0;
        double max_dd = 0.0;
        int32_t max_abs_inv = 0;
        std::vector<double> pnl_samples;
        std::vector<double> inv_samples;
        pnl_samples.reserve(1000);
        inv_samples.reserve(1000);

        MarketSnapshot last_snap{0, fair_price, book.best_bid(), book.best_ask(), 0, 0};

        while (!pq.empty()) {
            SimEvent ev = pq.top();
            pq.pop();
            if (ev.ts > cfg.duration_ns) break;

            switch (ev.type) {
                case EventType::BackgroundLP: {
                    Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
                    int off = lp_offset_dist(rng);
                    Price p = clamp_price(static_cast<Price>(std::llround(fair_price)) + (side == Side::Buy ? -off : off));
                    OrderId id = next_bg_id++;
                    book.add_order_zero_alloc({id, p, lp_qty_dist(rng), ev.ts, side, OrderType::Limit}, handle_trade);
                    bg_orders.push_back(id);

                    while (bg_orders.size() > 250) {
                        book.cancel_order(bg_orders.front());
                        bg_orders.pop_front();
                    }

                    Timestamp next_ts = ev.ts + std::max<Timestamp>(1, static_cast<Timestamp>(lp_iat(rng)));
                    push_event({next_ts, 0, EventType::BackgroundLP});
                    break;
                }

                case EventType::RetailMarketOrder: {
                    Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
                    OrderId id = next_bg_id++;
                    book.add_order_zero_alloc({id, 0, retail_qty_dist(rng), ev.ts, side, OrderType::Market}, handle_trade);
                    publish_md(ev.ts);

                    Timestamp next_ts = ev.ts + std::max<Timestamp>(1, static_cast<Timestamp>(retail_iat(rng)));
                    push_event({next_ts, 0, EventType::RetailMarketOrder});
                    break;
                }

                case EventType::FairPriceJump: {
                    int dir = side_dist(rng) == 0 ? 1 : -1;
                    int mag = jump_mag_dist(rng);
                    fair_price = static_cast<double>(clamp_price(static_cast<Price>(std::llround(fair_price)) + dir * mag));

                    publish_md(ev.ts);

                    Timestamp sniper_delay = static_cast<Timestamp>(
                        std::clamp(sniper_delay_dist(rng), 30'000.0, 1'500'000.0));
                    SimEvent sniper;
                    sniper.ts = ev.ts + sniper_delay;
                    sniper.type = EventType::InformedSniper;
                    sniper.side = (dir > 0) ? Side::Buy : Side::Sell;
                    sniper.price = static_cast<Price>(std::llround(fair_price));
                    sniper.qty = 25;
                    push_event(sniper);

                    Timestamp next_ts = ev.ts + std::max<Timestamp>(1, static_cast<Timestamp>(jump_iat(rng)));
                    push_event({next_ts, 0, EventType::FairPriceJump});
                    break;
                }

                case EventType::InformedSniper: {
                    OrderId id = next_bg_id++;
                    is_sniper_context = true;
                    book.add_order_zero_alloc({id, ev.price, ev.qty, ev.ts, ev.side, OrderType::IOC}, handle_trade);
                    is_sniper_context = false;
                    publish_md(ev.ts);
                    break;
                }

                case EventType::MarketDataToStrategy: {
                    last_snap = MarketSnapshot{
                        ev.ts, ev.ref_price, ev.snap_bid, ev.snap_ask, ev.snap_bid_vol, ev.snap_ask_vol
                    };
                    apply_strategy_quotes(ev.ts, last_snap);
                    break;
                }

                case EventType::FillToStrategy: {
                    if (ev.side == Side::Buy) {
                        strat_inv += static_cast<int32_t>(ev.qty);
                        if (ev.order_id == active_bid_id) {
                            active_bid_id = 0;
                            quoted_bid_price = INVALID_PRICE;
                        }
                    } else {
                        strat_inv -= static_cast<int32_t>(ev.qty);
                        if (ev.order_id == active_ask_id) {
                            active_ask_id = 0;
                            quoted_ask_price = INVALID_PRICE;
                        }
                    }
                    apply_strategy_quotes(ev.ts, last_snap);
                    break;
                }

                case EventType::StrategyAdd: {
                    book.add_order_zero_alloc({ev.order_id, ev.price, ev.qty, ev.ts, ev.side, OrderType::Limit}, handle_trade);
                    break;
                }

                case EventType::StrategyCancel: {
                    book.cancel_order(ev.order_id);
                    break;
                }

                case EventType::SampleMetrics: {
                    double mtm = true_cash + static_cast<double>(true_inv) * fair_price;
                    peak_pnl = std::max(peak_pnl, mtm);
                    max_dd = std::max(max_dd, peak_pnl - mtm);
                    max_abs_inv = std::max(max_abs_inv, std::abs(true_inv));

                    pnl_samples.push_back(mtm);
                    inv_samples.push_back(static_cast<double>(true_inv));

                    if (ts_csv.is_open()) {
                        ts_csv << (static_cast<double>(ev.ts) / 1e9) << ","
                               << fair_price << ","
                               << book.best_bid() << ","
                               << book.best_ask() << ","
                               << true_inv << ","
                               << mtm << "\n";
                    }

                    push_event({ev.ts + SAMPLE_STEP_NS, 0, EventType::SampleMetrics});
                    break;
                }
            }
        }

        double final_pnl = true_cash + static_cast<double>(true_inv) * fair_price;
        peak_pnl = std::max(peak_pnl, final_pnl);
        max_dd = std::max(max_dd, peak_pnl - final_pnl);
        max_abs_inv = std::max(max_abs_inv, std::abs(true_inv));

        double sharpe = 0.0;
        if (pnl_samples.size() > 2) {
            std::vector<double> rets;
            rets.reserve(pnl_samples.size() - 1);
            double sum = 0.0;
            for (size_t i = 1; i < pnl_samples.size(); ++i) {
                double r = pnl_samples[i] - pnl_samples[i - 1];
                rets.push_back(r);
                sum += r;
            }
            double mean = sum / static_cast<double>(rets.size());
            double sq_sum = 0.0;
            for (double r : rets) sq_sum += (r - mean) * (r - mean);
            double stdev = std::sqrt(sq_sum / static_cast<double>(rets.size() - 1));
            if (stdev > 1e-9) {
                sharpe = (mean / stdev) * std::sqrt(static_cast<double>(rets.size()));
            }
        }

        double inv_std = 0.0;
        if (inv_samples.size() > 1) {
            double mean_i = 0.0;
            for (double x : inv_samples) mean_i += x;
            mean_i /= static_cast<double>(inv_samples.size());
            double sq = 0.0;
            for (double x : inv_samples) sq += (x - mean_i) * (x - mean_i);
            inv_std = std::sqrt(sq / static_cast<double>(inv_samples.size() - 1));
        }

        return SimResult{
            strategy.name(),
            static_cast<double>(cfg.latency_ns) / 1000.0,
            final_pnl,
            max_dd,
            sharpe,
            inv_std,
            max_abs_inv,
            true_inv,
            total_fills,
            adverse_fills,
            traded_volume
        };
    }
};

} // namespace lob