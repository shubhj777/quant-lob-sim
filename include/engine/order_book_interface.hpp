#pragma once

#include "types.hpp"
#include <vector>

namespace lob {

class IOrderBook {
public:
    virtual ~IOrderBook() = default;

    virtual std::vector<Trade> add_order(const OrderRequest& req) = 0;
    virtual bool cancel_order(OrderId order_id) = 0;
    virtual std::vector<Trade> modify_order(OrderId order_id, Price new_price, Quantity new_qty, Timestamp ts) = 0;

    virtual Price    best_bid() const = 0;
    virtual Price    best_ask() const = 0;
    virtual Quantity bid_volume_at(Price price) const = 0;
    virtual Quantity ask_volume_at(Price price) const = 0;
    virtual size_t   order_count() const = 0;
};

} // namespace lob