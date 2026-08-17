#pragma once
#include <cstdint>
#include "order.hpp"
#include "types.hpp"
#include "order_side.hpp"
#include <list>

// Where a resting order lives, stored as a direct path rather than as keys to
// look up later.
//
//   level          -> the price level's FIFO list. A raw pointer is safe here
//                     because std::map nodes never relocate, and a level is
//                     only erased from the map once it is EMPTY -- at which
//                     point, by definition, no resting order and therefore no
//                     Locator can still reference it.
//   order_iterator -> the exact list node. Stable across erasure of any other
//                     node in the same list.
//
// side/price are retained only for the rare path where a cancel empties a level
// and it must then be removed from the map.
struct Locator
{
    OrderSide side;
    Price price;
    std::list<Order>* level;
    std::list<Order>::iterator order_iterator;

    static Locator create(OrderSide side, Price price, std::list<Order>* level,
                          std::list<Order>::iterator order_iterator)
    {
        return Locator{side, price, level, order_iterator};
    }
};
