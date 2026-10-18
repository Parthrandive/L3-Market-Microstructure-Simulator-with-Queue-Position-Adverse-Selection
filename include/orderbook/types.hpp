#pragma once

#include <cstdint>
#include <string_view>

namespace l3 {

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

struct Version {
    static constexpr std::string_view name = "L3 Order Book & Simulator";
    static constexpr int major = 0;
    static constexpr int minor = 1;
};

} // namespace l3
