#include <gtest/gtest.h>
#include "engine/types.hpp"
#include "engine/order_book_interface.hpp"

TEST(SetupTest, TypesCompile) {
    lob::OrderRequest req{1, 10050, 10, 1000, lob::Side::Buy, lob::OrderType::Limit};
    EXPECT_EQ(req.price, 10050);
    EXPECT_EQ(req.quantity, 10u);
}