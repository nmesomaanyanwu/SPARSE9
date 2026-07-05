#pragma once

#include <cstdint>
#include "order_type.hpp"
#include "order_side.hpp"
#include "time_in_force.hpp"
#include "types.hpp"

struct Order
{
    OrderId order_id;
    OrderType type;
    OrderSide side;
    Price price;
    Quantity quantity;
    TimeInForce time_in_force;
};
