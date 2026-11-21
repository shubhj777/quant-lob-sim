#pragma once

#include <cstdint>
#include <limits>

namespace lob {

using OrderId   = uint64_t;
using Price     = int64_t;
using Quantity  = uint32_t;
using Timestamp = uint64_t;

constexpr Price INVALID_PRICE = std::numeric_limits<Price>::min();

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1,
    IOC    = 2,
    FOK    = 3
};

struct OrderRequest {
    OrderId   order_id;
    Price     price;
    Quantity  quantity;
    Timestamp timestamp;
    Side      side;
    OrderType type;
};

struct Trade {
    OrderId   maker_order_id;
    OrderId   taker_order_id;
    Price     price;
    Quantity  quantity;
    Timestamp timestamp;
    Side      taker_side;
};

struct BookLevel {
    Price    price;
    Quantity total_quantity;
    uint32_t order_count;
};

} // namespace lob