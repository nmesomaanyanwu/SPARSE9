// main.cpp -- a tiny hand-driven demo of the matching engine.
#include <iostream>

#include "order.hpp"
#include "order_book.hpp"

namespace
{
// Helper to build orders without spelling out every field at the call site.
Order make(OrderId id, OrderSide side, OrderType type, Price price, Quantity qty,
           TimeInForce tif = TimeInForce::GTC)
{
    return Order{id, type, side, price, qty, tif};
}

void print_trades(const std::vector<Trade>& trades)
{
    if (trades.empty())
    {
        std::cout << "  (no trades)\n";
        return;
    }
    for (const Trade& t : trades)
        std::cout << "  TRADE #" << t.trade_id << ": buy " << t.buy_order_id
                  << " x sell " << t.sell_order_id << "  " << t.quantity
                  << " @ " << t.price << '\n';
}

void print_top(const OrderBook& book)
{
    std::cout << "  book: best_bid=";
    if (auto b = book.best_bid()) std::cout << *b; else std::cout << "-";
    std::cout << "  best_ask=";
    if (auto a = book.best_ask()) std::cout << *a; else std::cout << "-";
    std::cout << '\n';
}
} // namespace

int main()
{
    OrderBook book;

    std::cout << "Resting two asks: 10@101 (id 1), 5@102 (id 2)\n";
    print_trades(book.submit(make(1, OrderSide::SELL, OrderType::LIMIT, 101, 10)));
    print_trades(book.submit(make(2, OrderSide::SELL, OrderType::LIMIT, 102, 5)));
    print_top(book);

    std::cout << "\nAggressive buy 12@102 (id 3) -- sweeps 101 then part of 102:\n";
    print_trades(book.submit(make(3, OrderSide::BUY, OrderType::LIMIT, 102, 12)));
    print_top(book);

    std::cout << "\nCancel remaining ask id 2:\n";
    std::cout << "  cancel returned " << std::boolalpha << book.cancel(2) << '\n';
    print_top(book);

    return 0;
}
