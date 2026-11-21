#pragma once

#include "order_book_interface.hpp"
#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

namespace lob {

class NaiveOrderBook final : public IOrderBook {
private:
    struct RestingOrder {
        OrderId   order_id;
        Price     price;
        Quantity  quantity;
        Timestamp timestamp;
        Side      side;
    };

    struct PriceLevel {
        std::list<RestingOrder> orders;
        Quantity total_volume = 0;
    };

    struct OrderLocator {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator iter;
    };

    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>>    asks_;
    std::unordered_map<OrderId, OrderLocator>        order_lookup_;

    template <typename BookMap>
    bool can_fill_fok(const BookMap& opp_book, const OrderRequest& req) const {
        uint64_t available = 0;
        for (const auto& [level_price, level] : opp_book) {
            if (req.side == Side::Buy && level_price > req.price) break;
            if (req.side == Side::Sell && level_price < req.price) break;
            available += level.total_volume;
            if (available >= req.quantity) return true;
        }
        return false;
    }

    template <typename BookMap>
    void match_against(BookMap& opp_book, const OrderRequest& req, Quantity& remaining, std::vector<Trade>& trades) {
        auto level_it = opp_book.begin();
        while (remaining > 0 && level_it != opp_book.end()) {
            Price level_price = level_it->first;
            if (req.type != OrderType::Market) {
                if (req.side == Side::Buy && level_price > req.price) break;
                if (req.side == Side::Sell && level_price < req.price) break;
            }

            PriceLevel& level = level_it->second;
            auto ord_it = level.orders.begin();
            while (remaining > 0 && ord_it != level.orders.end()) {
                Quantity fill_qty = std::min(remaining, ord_it->quantity);
                trades.push_back(Trade{
                    ord_it->order_id,
                    req.order_id,
                    level_price,
                    fill_qty,
                    req.timestamp,
                    req.side
                });

                remaining -= fill_qty;
                ord_it->quantity -= fill_qty;
                level.total_volume -= fill_qty;

                if (ord_it->quantity == 0) {
                    order_lookup_.erase(ord_it->order_id);
                    ord_it = level.orders.erase(ord_it);
                } else {
                    ++ord_it;
                }
            }

            if (level.orders.empty()) {
                level_it = opp_book.erase(level_it);
            } else {
                ++level_it;
            }
        }
    }

    template <typename BookMap>
    void insert_resting(BookMap& book, const OrderRequest& req, Quantity remaining) {
        PriceLevel& level = book[req.price];
        level.orders.push_back(RestingOrder{
            req.order_id,
            req.price,
            remaining,
            req.timestamp,
            req.side
        });
        level.total_volume += remaining;
        auto it = std::prev(level.orders.end());
        order_lookup_.emplace(req.order_id, OrderLocator{req.side, req.price, it});
    }

public:
    std::vector<Trade> add_order(const OrderRequest& req) override {
        if (req.quantity == 0 || order_lookup_.contains(req.order_id)) {
            return {};
        }

        if (req.type == OrderType::FOK) {
            bool fillable = (req.side == Side::Buy)
                ? can_fill_fok(asks_, req)
                : can_fill_fok(bids_, req);
            if (!fillable) return {};
        }

        std::vector<Trade> trades;
        Quantity remaining = req.quantity;

        if (req.side == Side::Buy) {
            match_against(asks_, req, remaining, trades);
        } else {
            match_against(bids_, req, remaining, trades);
        }

        if (remaining > 0 && req.type == OrderType::Limit) {
            if (req.side == Side::Buy) {
                insert_resting(bids_, req, remaining);
            } else {
                insert_resting(asks_, req, remaining);
            }
        }

        return trades;
    }

    bool cancel_order(OrderId order_id) override {
        auto it = order_lookup_.find(order_id);
        if (it == order_lookup_.end()) return false;

        OrderLocator loc = it->second;
        if (loc.side == Side::Buy) {
            auto level_it = bids_.find(loc.price);
            level_it->second.total_volume -= loc.iter->quantity;
            level_it->second.orders.erase(loc.iter);
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        } else {
            auto level_it = asks_.find(loc.price);
            level_it->second.total_volume -= loc.iter->quantity;
            level_it->second.orders.erase(loc.iter);
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }

        order_lookup_.erase(it);
        return true;
    }

    std::vector<Trade> modify_order(OrderId order_id, Price new_price, Quantity new_qty, Timestamp ts) override {
        auto it = order_lookup_.find(order_id);
        if (it == order_lookup_.end()) return {};

        if (new_qty == 0) {
            cancel_order(order_id);
            return {};
        }

        OrderLocator loc = it->second;
        if (loc.price == new_price && new_qty <= loc.iter->quantity) {
            Quantity diff = loc.iter->quantity - new_qty;
            loc.iter->quantity = new_qty;
            if (loc.side == Side::Buy) {
                bids_.at(loc.price).total_volume -= diff;
            } else {
                asks_.at(loc.price).total_volume -= diff;
            }
            return {};
        }

        Side side = loc.side;
        cancel_order(order_id);
        return add_order(OrderRequest{order_id, new_price, new_qty, ts, side, OrderType::Limit});
    }

    Price best_bid() const override {
        return bids_.empty() ? INVALID_PRICE : bids_.begin()->first;
    }

    Price best_ask() const override {
        return asks_.empty() ? INVALID_PRICE : asks_.begin()->first;
    }

    Quantity bid_volume_at(Price price) const override {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_volume;
    }

    Quantity ask_volume_at(Price price) const override {
        auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second.total_volume;
    }

    size_t order_count() const override {
        return order_lookup_.size();
    }
};

} // namespace lob