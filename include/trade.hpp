#pragma once

#include <cstdint>
#include "order.hpp"
#include "types.hpp"

struct Trade
{
    TradeId trade_id;
    OrderId buy_order_id;
    OrderId sell_order_id;
    Price price;
    Quantity quantity;
};
