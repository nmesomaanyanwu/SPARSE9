#pragma once
#include <cstdint>
#include "order.hpp"
#include "types.hpp"
#include "order_side.hpp"

struct Locator
{
    OrderSide side;
    Price price;
    std::list<Order>::iterator order_iterator;

    static Locator create(OrderSide side, Price price, std::list<Order>::iterator order_iterator)
    {
        return Locator{side, price, order_iterator};
    }
};
