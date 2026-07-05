#pragma once

#include <cstdint>

// these are the different order strategies you can use when placing an order

enum class OrderType : std::int8_t
{
    LIMIT,
    MARKET
};
