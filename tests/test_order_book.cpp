#include <gtest/gtest.h>
#include "engine/naive_order_book.hpp"

using namespace lob;

TEST(NaiveOrderBookTest, EmptyBookDefaults) {
    NaiveOrderBook book;
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
    EXPECT_EQ(book.best_ask(), INVALID_PRICE);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.bid_volume_at(10000), 0u);
    EXPECT_EQ(book.ask_volume_at(10000), 0u);
}

TEST(NaiveOrderBookTest, RestingOrdersTrackBestPricesAndVolume) {
    NaiveOrderBook book;
    book.add_order({1, 10000, 10, 1, Side::Buy, OrderType::Limit});
    book.add_order({2, 10010, 20, 2, Side::Buy, OrderType::Limit});
    book.add_order({3, 10010, 15, 3, Side::Buy, OrderType::Limit});
    book.add_order({4, 10050, 30, 4, Side::Sell, OrderType::Limit});
    book.add_order({5, 10030, 25, 5, Side::Sell, OrderType::Limit});

    EXPECT_EQ(book.best_bid(), 10010);
    EXPECT_EQ(book.best_ask(), 10030);
    EXPECT_EQ(book.bid_volume_at(10010), 35u);
    EXPECT_EQ(book.ask_volume_at(10030), 25u);
    EXPECT_EQ(book.order_count(), 5u);
}

TEST(NaiveOrderBookTest, ExactLimitMatchExecutesAtMakerPrice) {
    NaiveOrderBook book;
    book.add_order({1, 10050, 10, 1, Side::Sell, OrderType::Limit});

    auto trades = book.add_order({2, 10080, 10, 2, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
    EXPECT_EQ(trades[0].taker_order_id, 2u);
    EXPECT_EQ(trades[0].price, 10050);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_ask(), INVALID_PRICE);
}

TEST(NaiveOrderBookTest, PriceTimePriorityFIFO) {
    NaiveOrderBook book;
    book.add_order({1, 10050, 10, 1, Side::Sell, OrderType::Limit});
    book.add_order({2, 10050, 10, 2, Side::Sell, OrderType::Limit});
    book.add_order({3, 10040, 10, 3, Side::Sell, OrderType::Limit});

    auto trades = book.add_order({4, 10050, 15, 4, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_order_id, 3u);
    EXPECT_EQ(trades[0].price, 10040);
    EXPECT_EQ(trades[0].quantity, 10u);

    EXPECT_EQ(trades[1].maker_order_id, 1u);
    EXPECT_EQ(trades[1].price, 10050);
    EXPECT_EQ(trades[1].quantity, 5u);

    EXPECT_EQ(book.ask_volume_at(10050), 15u);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(NaiveOrderBookTest, PartialFillAndRestRemainder) {
    NaiveOrderBook book;
    book.add_order({1, 10020, 10, 1, Side::Sell, OrderType::Limit});

    auto trades = book.add_order({2, 10020, 25, 2, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(book.best_ask(), INVALID_PRICE);
    EXPECT_EQ(book.best_bid(), 10020);
    EXPECT_EQ(book.bid_volume_at(10020), 15u);
}

TEST(NaiveOrderBookTest, MarketOrderSweepsLevelsWithoutResting) {
    NaiveOrderBook book;
    book.add_order({1, 10010, 10, 1, Side::Sell, OrderType::Limit});
    book.add_order({2, 10020, 15, 2, Side::Sell, OrderType::Limit});

    auto trades = book.add_order({3, 0, 50, 3, Side::Buy, OrderType::Market});
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[1].quantity, 15u);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_bid(), INVALID_PRICE);
}

TEST(NaiveOrderBookTest, IOCFillsAvailableAndDiscardsRest) {
    NaiveOrderBook book;
    book.add_order({1, 10010, 10, 1, Side::Sell, OrderType::Limit});

    auto trades = book.add_order({2, 10010, 30, 2, Side::Buy, OrderType::IOC});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(NaiveOrderBookTest, FOKAllOrNothing) {
    NaiveOrderBook book;
    book.add_order({1, 10010, 10, 1, Side::Sell, OrderType::Limit});
    book.add_order({2, 10020, 10, 2, Side::Sell, OrderType::Limit});

    auto failed = book.add_order({3, 10020, 25, 3, Side::Buy, OrderType::FOK});
    EXPECT_TRUE(failed.empty());
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.ask_volume_at(10010), 10u);

    auto filled = book.add_order({4, 10020, 20, 4, Side::Buy, OrderType::FOK});
    ASSERT_EQ(filled.size(), 2u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(NaiveOrderBookTest, CancelUpdatesBestPriceAndHandlesMissing) {
    NaiveOrderBook book;
    book.add_order({1, 10000, 10, 1, Side::Buy, OrderType::Limit});
    book.add_order({2, 10010, 20, 2, Side::Buy, OrderType::Limit});

    EXPECT_TRUE(book.cancel_order(2));
    EXPECT_FALSE(book.cancel_order(999));
    EXPECT_EQ(book.best_bid(), 10000);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(NaiveOrderBookTest, ModifyQtyDownPreservesTimePriority) {
    NaiveOrderBook book;
    book.add_order({1, 10050, 20, 1, Side::Sell, OrderType::Limit});
    book.add_order({2, 10050, 20, 2, Side::Sell, OrderType::Limit});

    auto mod_trades = book.modify_order(1, 10050, 10, 3);
    EXPECT_TRUE(mod_trades.empty());

    auto trades = book.add_order({3, 10050, 10, 4, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 1u);
}

TEST(NaiveOrderBookTest, ModifyQtyUpLosesTimePriority) {
    NaiveOrderBook book;
    book.add_order({1, 10050, 10, 1, Side::Sell, OrderType::Limit});
    book.add_order({2, 10050, 10, 2, Side::Sell, OrderType::Limit});

    book.modify_order(1, 10050, 15, 3);

    auto trades = book.add_order({3, 10050, 10, 4, Side::Buy, OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 2u);
}

TEST(NaiveOrderBookTest, ModifyPriceAcrossSpreadGeneratesTrades) {
    NaiveOrderBook book;
    book.add_order({1, 10000, 10, 1, Side::Buy, OrderType::Limit});
    book.add_order({2, 10050, 10, 2, Side::Sell, OrderType::Limit});

    auto trades = book.modify_order(1, 10050, 10, 3);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, 2u);
    EXPECT_EQ(trades[0].taker_order_id, 1u);
    EXPECT_EQ(book.order_count(), 0u);
}