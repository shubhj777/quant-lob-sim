#pragma once

#include "engine/types.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace lob {

struct MarketSnapshot {
    Timestamp timestamp = 0;
    double    reference_price = 50000.0;
    Price     best_bid = INVALID_PRICE;
    Price     best_ask = INVALID_PRICE;
    Quantity  bid_vol = 0;
    Quantity  ask_vol = 0;
};

struct QuoteDecision {
    bool     quote_bid = false;
    bool     quote_ask = false;
    Price    bid_price = INVALID_PRICE;
    Price    ask_price = INVALID_PRICE;
    Quantity bid_qty = 0;
    Quantity ask_qty = 0;
};

class IMarketMaker {
public:
    virtual ~IMarketMaker() = default;
    virtual std::string name() const = 0;
    virtual void reset() = 0;
    virtual QuoteDecision on_market_update(const MarketSnapshot& snap, int32_t inventory, double time_fraction) = 0;
};

class NaiveSymmetricMM final : public IMarketMaker {
private:
    int64_t  half_spread_;
    Quantity quote_size_;
    int32_t  max_inventory_;

public:
    NaiveSymmetricMM(int64_t half_spread = 1, Quantity quote_size = 5, int32_t max_inventory = 100)
        : half_spread_(half_spread), quote_size_(quote_size), max_inventory_(max_inventory) {}

    std::string name() const override { return "NaiveSymmetricMM"; }

    void reset() override {}

    QuoteDecision on_market_update(const MarketSnapshot& snap, int32_t inventory, double /*time_fraction*/) override {
        QuoteDecision q;
        Price center = static_cast<Price>(std::llround(snap.reference_price));
        q.bid_price = center - half_spread_;
        q.ask_price = center + half_spread_;
        q.bid_qty = quote_size_;
        q.ask_qty = quote_size_;
        q.quote_bid = (inventory < max_inventory_);
        q.quote_ask = (inventory > -max_inventory_);
        return q;
    }
};

class AvellanedaStoikovMM final : public IMarketMaker {
private:
    double   gamma_;
    double   kappa_;
    double   obi_weight_;
    Quantity quote_size_;
    int32_t  max_inventory_;
    double   ewma_var_ = 2.0;
    double   last_ref_ = -1.0;

public:
    AvellanedaStoikovMM(double gamma = 0.06,
                        double kappa = 1.2,
                        double obi_weight = 0.35,
                        Quantity quote_size = 5,
                        int32_t max_inventory = 40)
        : gamma_(gamma),
          kappa_(kappa),
          obi_weight_(obi_weight),
          quote_size_(quote_size),
          max_inventory_(max_inventory) {}

    std::string name() const override { return "AvellanedaStoikovMM"; }

    void reset() override {
        ewma_var_ = 2.0;
        last_ref_ = -1.0;
    }

    QuoteDecision on_market_update(const MarketSnapshot& snap, int32_t inventory, double time_fraction) override {
        if (last_ref_ > 0.0) {
            double diff = snap.reference_price - last_ref_;
            if (std::abs(diff) > 1e-9) {
                ewma_var_ = 0.94 * ewma_var_ + 0.06 * (diff * diff);
            }
        }
        last_ref_ = snap.reference_price;

        double sigma2 = std::clamp(ewma_var_, 1.0, 12.0);
        double tau = std::max(0.25, 1.0 - 0.5 * time_fraction);

        double obi = 0.0;
        double total_top = static_cast<double>(snap.bid_vol + snap.ask_vol);
        if (total_top > 0.0) {
            obi = (static_cast<double>(snap.bid_vol) - static_cast<double>(snap.ask_vol)) / total_top;
        }

        double eff_mid = snap.reference_price + obi_weight_ * obi;
        double reservation = eff_mid - static_cast<double>(inventory) * gamma_ * sigma2 * tau;
        double half_spread = 0.5 * gamma_ * sigma2 * tau + (1.0 / gamma_) * std::log(1.0 + gamma_ / kappa_);
        half_spread = std::clamp(half_spread, 1.0, 5.0);

        Price bid = static_cast<Price>(std::floor(reservation - half_spread));
        Price ask = static_cast<Price>(std::ceil(reservation + half_spread));

        if (bid >= ask) {
            bid = ask - 1;
        }

        QuoteDecision q;
        q.bid_price = bid;
        q.ask_price = ask;
        q.bid_qty = quote_size_;
        q.ask_qty = quote_size_;
        q.quote_bid = (inventory < max_inventory_);
        q.quote_ask = (inventory > -max_inventory_);
        return q;
    }
};

} // namespace lob