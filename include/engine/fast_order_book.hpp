#pragma once

#include "flat_hash_map.hpp"
#include "memory_pool.hpp"
#include "order_book_interface.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

namespace lob {

class FastOrderBook final : public IOrderBook {
private:
    struct FastOrder {
        OrderId   order_id = 0;
        Price     price = 0;
        Timestamp timestamp = 0;
        Quantity  quantity = 0;
        int32_t   prev_idx = -1;
        int32_t   next_idx = -1;
        Side      side = Side::Buy;
    };

    struct PriceLevel {
        int32_t  head_idx = -1;
        int32_t  tail_idx = -1;
        Quantity total_volume = 0;
        uint32_t order_count = 0;
    };

    Price min_price_;
    Price max_price_;
    int32_t num_levels_;
    size_t num_words_;

    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    std::vector<uint64_t> bid_bitmap_;
    std::vector<uint64_t> ask_bitmap_;

    MemoryPool<FastOrder> pool_;
    FlatHashMap order_lookup_;

    int32_t best_bid_idx_ = -1;
    int32_t best_ask_idx_ = -1;

    inline void set_bid_bit(int32_t idx) {
        bid_bitmap_[static_cast<size_t>(idx >> 6)] |= (1ULL << (idx & 63));
    }

    inline void clear_bid_bit(int32_t idx) {
        bid_bitmap_[static_cast<size_t>(idx >> 6)] &= ~(1ULL << (idx & 63));
    }

    inline void set_ask_bit(int32_t idx) {
        ask_bitmap_[static_cast<size_t>(idx >> 6)] |= (1ULL << (idx & 63));
    }

    inline void clear_ask_bit(int32_t idx) {
        ask_bitmap_[static_cast<size_t>(idx >> 6)] &= ~(1ULL << (idx & 63));
    }

    int32_t find_next_ask(int32_t start_idx) const {
        if (start_idx < 0 || start_idx >= num_levels_) return -1;
        size_t word = static_cast<size_t>(start_idx >> 6);
        int bit = start_idx & 63;

        uint64_t w = ask_bitmap_[word] & (~0ULL << bit);
        if (w != 0) {
            return static_cast<int32_t>((word << 6) + static_cast<size_t>(std::countr_zero(w)));
        }

        for (++word; word < num_words_; ++word) {
            if (ask_bitmap_[word] != 0) {
                return static_cast<int32_t>((word << 6) + static_cast<size_t>(std::countr_zero(ask_bitmap_[word])));
            }
        }
        return -1;
    }

    int32_t find_prev_bid(int32_t start_idx) const {
        if (start_idx < 0 || start_idx >= num_levels_) return -1;
        int64_t word = static_cast<int64_t>(start_idx >> 6);
        int bit = start_idx & 63;

        uint64_t mask = (bit == 63) ? ~0ULL : ((1ULL << (bit + 1)) - 1ULL);
        uint64_t w = bid_bitmap_[static_cast<size_t>(word)] & mask;
        if (w != 0) {
            return static_cast<int32_t>((word << 6) + (63 - std::countl_zero(w)));
        }

        for (--word; word >= 0; --word) {
            if (bid_bitmap_[static_cast<size_t>(word)] != 0) {
                return static_cast<int32_t>((word << 6) + (63 - std::countl_zero(bid_bitmap_[static_cast<size_t>(word)])));
            }
        }
        return -1;
    }

    bool can_fill_fok(const OrderRequest& req) const {
        uint64_t available = 0;
        if (req.side == Side::Buy) {
            int32_t idx = best_ask_idx_;
            while (idx != -1) {
                Price level_price = min_price_ + idx;
                if (level_price > req.price) break;
                available += asks_[static_cast<size_t>(idx)].total_volume;
                if (available >= req.quantity) return true;
                idx = find_next_ask(idx + 1);
            }
        } else {
            int32_t idx = best_bid_idx_;
            while (idx != -1) {
                Price level_price = min_price_ + idx;
                if (level_price < req.price) break;
                available += bids_[static_cast<size_t>(idx)].total_volume;
                if (available >= req.quantity) return true;
                idx = find_prev_bid(idx - 1);
            }
        }
        return false;
    }

    template <typename OnTrade>
    void match_buy(const OrderRequest& req, Quantity& remaining, OnTrade&& on_trade) {
        while (remaining > 0 && best_ask_idx_ != -1) {
            Price level_price = min_price_ + best_ask_idx_;
            if (req.type != OrderType::Market && level_price > req.price) break;

            PriceLevel& level = asks_[static_cast<size_t>(best_ask_idx_)];
            int32_t ord_idx = level.head_idx;

            while (remaining > 0 && ord_idx != -1) {
                FastOrder& maker = pool_[ord_idx];
                int32_t next_idx = maker.next_idx;
                Quantity fill_qty = std::min(remaining, maker.quantity);

                on_trade(Trade{
                    maker.order_id,
                    req.order_id,
                    level_price,
                    fill_qty,
                    req.timestamp,
                    req.side
                });

                remaining -= fill_qty;
                maker.quantity -= fill_qty;
                level.total_volume -= fill_qty;

                if (maker.quantity == 0) {
                    level.head_idx = next_idx;
                    if (next_idx != -1) {
                        pool_[next_idx].prev_idx = -1;
                    } else {
                        level.tail_idx = -1;
                    }
                    --level.order_count;
                    order_lookup_.erase(maker.order_id);
                    pool_.deallocate(ord_idx);
                }
                ord_idx = next_idx;
            }

            if (level.order_count == 0) {
                clear_ask_bit(best_ask_idx_);
                best_ask_idx_ = find_next_ask(best_ask_idx_ + 1);
            } else {
                break;
            }
        }
    }

    template <typename OnTrade>
    void match_sell(const OrderRequest& req, Quantity& remaining, OnTrade&& on_trade) {
        while (remaining > 0 && best_bid_idx_ != -1) {
            Price level_price = min_price_ + best_bid_idx_;
            if (req.type != OrderType::Market && level_price < req.price) break;

            PriceLevel& level = bids_[static_cast<size_t>(best_bid_idx_)];
            int32_t ord_idx = level.head_idx;

            while (remaining > 0 && ord_idx != -1) {
                FastOrder& maker = pool_[ord_idx];
                int32_t next_idx = maker.next_idx;
                Quantity fill_qty = std::min(remaining, maker.quantity);

                on_trade(Trade{
                    maker.order_id,
                    req.order_id,
                    level_price,
                    fill_qty,
                    req.timestamp,
                    req.side
                });

                remaining -= fill_qty;
                maker.quantity -= fill_qty;
                level.total_volume -= fill_qty;

                if (maker.quantity == 0) {
                    level.head_idx = next_idx;
                    if (next_idx != -1) {
                        pool_[next_idx].prev_idx = -1;
                    } else {
                        level.tail_idx = -1;
                    }
                    --level.order_count;
                    order_lookup_.erase(maker.order_id);
                    pool_.deallocate(ord_idx);
                }
                ord_idx = next_idx;
            }

            if (level.order_count == 0) {
                clear_bid_bit(best_bid_idx_);
                best_bid_idx_ = find_prev_bid(best_bid_idx_ - 1);
            } else {
                break;
            }
        }
    }

    void insert_resting(const OrderRequest& req, Quantity remaining) {
        assert(req.price >= min_price_ && req.price <= max_price_);
        int32_t p_idx = static_cast<int32_t>(req.price - min_price_);
        int32_t ord_idx = pool_.allocate();

        PriceLevel& level = (req.side == Side::Buy)
            ? bids_[static_cast<size_t>(p_idx)]
            : asks_[static_cast<size_t>(p_idx)];

        FastOrder& ord = pool_[ord_idx];
        ord.order_id  = req.order_id;
        ord.price     = req.price;
        ord.timestamp = req.timestamp;
        ord.quantity  = remaining;
        ord.side      = req.side;
        ord.prev_idx  = level.tail_idx;
        ord.next_idx  = -1;

        if (level.tail_idx != -1) {
            pool_[level.tail_idx].next_idx = ord_idx;
        } else {
            level.head_idx = ord_idx;
        }
        level.tail_idx = ord_idx;
        level.total_volume += remaining;
        ++level.order_count;

        if (level.order_count == 1) {
            if (req.side == Side::Buy) {
                set_bid_bit(p_idx);
                if (best_bid_idx_ == -1 || p_idx > best_bid_idx_) {
                    best_bid_idx_ = p_idx;
                }
            } else {
                set_ask_bit(p_idx);
                if (best_ask_idx_ == -1 || p_idx < best_ask_idx_) {
                    best_ask_idx_ = p_idx;
                }
            }
        }

        order_lookup_.insert(req.order_id, ord_idx);
    }

public:
    explicit FastOrderBook(Price min_price = 0, Price max_price = 100000, size_t max_orders = 262144)
        : min_price_(min_price),
          max_price_(max_price),
          num_levels_(static_cast<int32_t>(max_price - min_price + 1)),
          num_words_(static_cast<size_t>((num_levels_ + 63) / 64)),
          bids_(static_cast<size_t>(num_levels_)),
          asks_(static_cast<size_t>(num_levels_)),
          bid_bitmap_(num_words_, 0),
          ask_bitmap_(num_words_, 0),
          pool_(max_orders),
          order_lookup_(max_orders) {}

    template <typename OnTrade>
    void add_order_zero_alloc(const OrderRequest& req, OnTrade&& on_trade) {
        if (req.quantity == 0 || order_lookup_.contains(req.order_id)) {
            return;
        }

        if (req.type == OrderType::FOK && !can_fill_fok(req)) {
            return;
        }

        Quantity remaining = req.quantity;
        if (req.side == Side::Buy) {
            match_buy(req, remaining, on_trade);
        } else {
            match_sell(req, remaining, on_trade);
        }

        if (remaining > 0 && req.type == OrderType::Limit) {
            insert_resting(req, remaining);
        }
    }

    std::vector<Trade> add_order(const OrderRequest& req) override {
        std::vector<Trade> trades;
        add_order_zero_alloc(req, [&trades](const Trade& t) {
            trades.push_back(t);
        });
        return trades;
    }

    bool cancel_order(OrderId order_id) override {
        int32_t ord_idx = order_lookup_.find(order_id);
        if (ord_idx == -1) return false;

        FastOrder& ord = pool_[ord_idx];
        int32_t p_idx = static_cast<int32_t>(ord.price - min_price_);
        PriceLevel& level = (ord.side == Side::Buy)
            ? bids_[static_cast<size_t>(p_idx)]
            : asks_[static_cast<size_t>(p_idx)];

        if (ord.prev_idx != -1) {
            pool_[ord.prev_idx].next_idx = ord.next_idx;
        } else {
            level.head_idx = ord.next_idx;
        }

        if (ord.next_idx != -1) {
            pool_[ord.next_idx].prev_idx = ord.prev_idx;
        } else {
            level.tail_idx = ord.prev_idx;
        }

        level.total_volume -= ord.quantity;
        --level.order_count;

        if (level.order_count == 0) {
            if (ord.side == Side::Buy) {
                clear_bid_bit(p_idx);
                if (p_idx == best_bid_idx_) {
                    best_bid_idx_ = find_prev_bid(p_idx - 1);
                }
            } else {
                clear_ask_bit(p_idx);
                if (p_idx == best_ask_idx_) {
                    best_ask_idx_ = find_next_ask(p_idx + 1);
                }
            }
        }

        order_lookup_.erase(order_id);
        pool_.deallocate(ord_idx);
        return true;
    }

    template <typename OnTrade>
    void modify_order_zero_alloc(OrderId order_id, Price new_price, Quantity new_qty, Timestamp ts, OnTrade&& on_trade) {
        int32_t ord_idx = order_lookup_.find(order_id);
        if (ord_idx == -1) return;

        if (new_qty == 0) {
            cancel_order(order_id);
            return;
        }

        FastOrder& ord = pool_[ord_idx];
        if (ord.price == new_price && new_qty <= ord.quantity) {
            Quantity diff = ord.quantity - new_qty;
            ord.quantity = new_qty;
            int32_t p_idx = static_cast<int32_t>(ord.price - min_price_);
            if (ord.side == Side::Buy) {
                bids_[static_cast<size_t>(p_idx)].total_volume -= diff;
            } else {
                asks_[static_cast<size_t>(p_idx)].total_volume -= diff;
            }
            return;
        }

        Side side = ord.side;
        cancel_order(order_id);
        add_order_zero_alloc(OrderRequest{order_id, new_price, new_qty, ts, side, OrderType::Limit}, on_trade);
    }

    std::vector<Trade> modify_order(OrderId order_id, Price new_price, Quantity new_qty, Timestamp ts) override {
        std::vector<Trade> trades;
        modify_order_zero_alloc(order_id, new_price, new_qty, ts, [&trades](const Trade& t) {
            trades.push_back(t);
        });
        return trades;
    }

    Price best_bid() const override {
        return best_bid_idx_ == -1 ? INVALID_PRICE : min_price_ + best_bid_idx_;
    }

    Price best_ask() const override {
        return best_ask_idx_ == -1 ? INVALID_PRICE : min_price_ + best_ask_idx_;
    }

    Quantity bid_volume_at(Price price) const override {
        if (price < min_price_ || price > max_price_) return 0;
        return bids_[static_cast<size_t>(price - min_price_)].total_volume;
    }

    Quantity ask_volume_at(Price price) const override {
        if (price < min_price_ || price > max_price_) return 0;
        return asks_[static_cast<size_t>(price - min_price_)].total_volume;
    }

    size_t order_count() const override {
        return order_lookup_.size();
    }
};

} // namespace lob