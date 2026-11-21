#include <gtest/gtest.h>
#include "engine/fast_order_book.hpp"
#include "engine/naive_order_book.hpp"
#include <random>
#include <vector>

using namespace lob;

template <typename BookT>
class OrderBookTest : public ::testing::Test {
protected:
    BookT book;
};

using BookImplementations = ::testing::Types<NaiveOrderBook, FastOrderBook>;
TYPED_TEST_SUITE(OrderBookTest, BookImplementations);

TYPED_TEST(OrderBookTest, EmptyBookDefaults) {
    EXPECT_EQ(this->book.best_bid(), INVALID_PRICE);
    EXPECT_EQ(this->book.best_ask(), INVALID_PRICE);
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_EQ(this->book.bid_volume_at(10000), 0u);
    EXPECT_EQ(this->book.ask_volume_at(10000), 0u);
}

TYPED_TEST(OrderBookTest, RestingOrdersTrackBestPricesAndVolume) {
    this->book.add_order({1, 10000, 10, 1, Side::Buy, OrderType::Limit});
    this->book.add_order({2, 10010, 20, 2, Side::Buy, OrderType::Limit});
    this->book.add_order({3, 10010, 15, 3, Side::Buy, OrderType::Limit});
    this->book.add_order({4, 10050, 30, 4, Side::Sell, OrderType::Limit});
    this->book.add_order({5, 10030, 25, 5, Side::Sell, OrderType::Limit});

    EXPECT_EQ(this->book.best_bid(), 10010);
    EXPECT_EQ(this->book.best_ask(), 10030);
    EXPECT_EQ(this->book.bid_volume_at(10010), 35u);
    EXPECT_EQ(this->book.ask_volume_at(10030), 25u);
    EXPECT_EQ(this->book.order_count(), 5u);
}

TYPED_TEST(OrderBookTest, ExactLimitMatchExecutesAtMakerPrice) {
    this->book.add_order({1, 10050, 10, 1, Side::Sell, OrderType::Limit});

    auto trades = this->book.add_order({2, 10080, 10, 2, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
    EXPECT_EQ(trades[0].taker_order_id, 2u);
    EXPECT_EQ(trades[0].price, 10050);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_EQ(this->book.best_ask(), INVALID_PRICE);
}

TYPED_TEST(OrderBookTest, PriceTimePriorityFIFO) {
    this->book.add_order({1, 10050, 10, 1, Side::Sell, OrderType::Limit});
    this->book.add_order({2, 10050, 10, 2, Side::Sell, OrderType::Limit});
    this->book.add_order({3, 10040, 10, 3, Side::Sell, OrderType::Limit});

    auto trades = this->book.add_order({4, 10050, 15, 4, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_order_id, 3u);
    EXPECT_EQ(trades[0].price, 10040);
    EXPECT_EQ(trades[0].quantity, 10u);

    EXPECT_EQ(trades[1].maker_order_id, 1u);
    EXPECT_EQ(trades[1].price, 10050);
    EXPECT_EQ(trades[1].quantity, 5u);

    EXPECT_EQ(this->book.ask_volume_at(10050), 15u);
    EXPECT_EQ(this->book.order_count(), 2u);
}

TYPED_TEST(OrderBookTest, PartialFillAndRestRemainder) {
    this->book.add_order({1, 10020, 10, 1, Side::Sell, OrderType::Limit});

    auto trades = this->book.add_order({2, 10020, 25, 2, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(this->book.best_ask(), INVALID_PRICE);
    EXPECT_EQ(this->book.best_bid(), 10020);
    EXPECT_EQ(this->book.bid_volume_at(10020), 15u);
}

TYPED_TEST(OrderBookTest, MarketOrderSweepsLevelsWithoutResting) {
    this->book.add_order({1, 10010, 10, 1, Side::Sell, OrderType::Limit});
    this->book.add_order({2, 10020, 15, 2, Side::Sell, OrderType::Limit});

    auto trades = this->book.add_order({3, 0, 50, 3, Side::Buy, OrderType::Market});
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[1].quantity, 15u);
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_EQ(this->book.best_bid(), INVALID_PRICE);
}

TYPED_TEST(OrderBookTest, IOCFillsAvailableAndDiscardsRest) {
    this->book.add_order({1, 10010, 10, 1, Side::Sell, OrderType::Limit});

    auto trades = this->book.add_order({2, 10010, 30, 2, Side::Buy, OrderType::IOC});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, FOKAllOrNothing) {
    this->book.add_order({1, 10010, 10, 1, Side::Sell, OrderType::Limit});
    this->book.add_order({2, 10020, 10, 2, Side::Sell, OrderType::Limit});

    auto failed = this->book.add_order({3, 10020, 25, 3, Side::Buy, OrderType::FOK});
    EXPECT_TRUE(failed.empty());
    EXPECT_EQ(this->book.order_count(), 2u);
    EXPECT_EQ(this->book.ask_volume_at(10010), 10u);

    auto filled = this->book.add_order({4, 10020, 20, 4, Side::Buy, OrderType::FOK});
    ASSERT_EQ(filled.size(), 2u);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, CancelUpdatesBestPriceAndHandlesMissing) {
    this->book.add_order({1, 10000, 10, 1, Side::Buy, OrderType::Limit});
    this->book.add_order({2, 10010, 20, 2, Side::Buy, OrderType::Limit});

    EXPECT_TRUE(this->book.cancel_order(2));
    EXPECT_FALSE(this->book.cancel_order(999));
    EXPECT_EQ(this->book.best_bid(), 10000);
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, ModifyQtyDownPreservesTimePriority) {
    this->book.add_order({1, 10050, 20, 1, Side::Sell, OrderType::Limit});
    this->book.add_order({2, 10050, 20, 2, Side::Sell, OrderType::Limit});

    auto mod_trades = this->book.modify_order(1, 10050, 10, 3);
    EXPECT_TRUE(mod_trades.empty());

    auto trades = this->book.add_order({3, 10050, 10, 4, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
}

TYPED_TEST(OrderBookTest, ModifyQtyUpLosesTimePriority) {
    this->book.add_order({1, 10050, 10, 1, Side::Sell, OrderType::Limit});
    this->book.add_order({2, 10050, 20, 2, Side::Sell, OrderType::Limit});

    this->book.modify_order(1, 10050, 15, 3);

    auto trades = this->book.add_order({3, 10050, 10, 4, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 2u);
}

TYPED_TEST(OrderBookTest, ModifyPriceAcrossSpreadGeneratesTrades) {
    this->book.add_order({1, 10000, 10, 1, Side::Buy, OrderType::Limit});
    this->book.add_order({2, 10050, 10, 2, Side::Sell, OrderType::Limit});

    auto trades = this->book.modify_order(1, 10050, 10, 3);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 2u);
    EXPECT_EQ(trades[0].taker_order_id, 1u);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TEST(DifferentialFuzzTest, FastMatchesNaiveAcross200kEvents) {
    NaiveOrderBook naive;
    FastOrderBook fast(9800, 10200, 300000);

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<Price> price_dist(9900, 10100);
    std::uniform_int_distribution<Quantity> qty_dist(1, 50);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::vector<OrderId> known_ids;
    known_ids.reserve(150000);
    OrderId next_id = 1;

    auto assert_same_trades = [](const std::vector<Trade>& a, const std::vector<Trade>& b) {
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].maker_order_id, b[i].maker_order_id);
            EXPECT_EQ(a[i].taker_order_id, b[i].taker_order_id);
            EXPECT_EQ(a[i].price, b[i].price);
            EXPECT_EQ(a[i].quantity, b[i].quantity);
            EXPECT_EQ(a[i].taker_side, b[i].taker_side);
        }
    };

    for (uint64_t step = 1; step <= 200000; ++step) {
        int action = action_dist(rng);

        if (action < 55 || known_ids.empty()) {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            OrderType type = OrderType::Limit;
            if (action >= 45 && action < 49) type = OrderType::Market;
            else if (action >= 49 && action < 52) type = OrderType::IOC;
            else if (action >= 52 && action < 55) type = OrderType::FOK;

            OrderRequest req{next_id++, price_dist(rng), qty_dist(rng), step, side, type};
            if (type == OrderType::Limit) {
                known_ids.push_back(req.order_id);
            }

            auto t_naive = naive.add_order(req);
            auto t_fast  = fast.add_order(req);
            assert_same_trades(t_naive, t_fast);
        } else if (action < 80) {
            OrderId target = known_ids[rng() % known_ids.size()];
            EXPECT_EQ(naive.cancel_order(target), fast.cancel_order(target));
        } else {
            OrderId target = known_ids[rng() % known_ids.size()];
            Price new_price = price_dist(rng);
            Quantity new_qty = (action < 85) ? 0 : qty_dist(rng);
            auto t_naive = naive.modify_order(target, new_price, new_qty, step);
            auto t_fast  = fast.modify_order(target, new_price, new_qty, step);
            assert_same_trades(t_naive, t_fast);
        }

        ASSERT_EQ(naive.best_bid(), fast.best_bid());
        ASSERT_EQ(naive.best_ask(), fast.best_ask());
        ASSERT_EQ(naive.order_count(), fast.order_count());

        if (fast.best_bid() != INVALID_PRICE && fast.best_ask() != INVALID_PRICE) {
            ASSERT_LT(fast.best_bid(), fast.best_ask());
        }
        if (fast.best_bid() != INVALID_PRICE) {
            ASSERT_EQ(naive.bid_volume_at(fast.best_bid()), fast.bid_volume_at(fast.best_bid()));
        }
        if (fast.best_ask() != INVALID_PRICE) {
            ASSERT_EQ(naive.ask_volume_at(fast.best_ask()), fast.ask_volume_at(fast.best_ask()));
        }
    }
}