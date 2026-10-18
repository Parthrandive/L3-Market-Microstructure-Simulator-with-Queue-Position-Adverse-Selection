#include <gtest/gtest.h>
#include "orderbook/types.hpp"

TEST(TypesTest, VersionCheck) {
    EXPECT_EQ(l3::Version::major, 0);
    EXPECT_EQ(l3::Version::minor, 1);
    EXPECT_FALSE(l3::Version::name.empty());
}

TEST(TypesTest, SideValues) {
    EXPECT_NE(static_cast<uint8_t>(l3::Side::Buy), static_cast<uint8_t>(l3::Side::Sell));
}
