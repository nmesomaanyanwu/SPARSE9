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

    static Trade create(OrderId buy_order_id, OrderId sell_order_id, Price price, Quantity quantity)
    {
        static TradeId next_trade_id = 1;
        return Trade{next_trade_id++, buy_order_id, sell_order_id, price, quantity};
    }
};
