// order_book.hpp
#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "locator.hpp"
#include "order.hpp"
#include "trade.hpp"
#include "types.hpp"

// A single-threaded, correctness-first matching engine.
//
// Design (deliberately "obvious and clear", not fast):
//   asks_  : Price ascending  -> begin() is the best (lowest) ask
//   bids_  : Price descending -> begin() is the best (highest) bid
//   index_ : OrderId -> Locator, so cancel() is O(1) instead of O(n)
//
// Ownership: the book owns every resting Order. A resting order lives inside
// the std::list at its price level. The Locator stored in index_ holds a
// std::list<Order>::iterator to that node -- valid because list iterators
// survive insertion/erasure of *other* nodes in the same list.
class OrderBook
{
public:
    // Submit an incoming order. Returns the trades it generated (possibly empty).
    // The order is passed by value because matching mutates its remaining
    // quantity, and any unfilled remainder may rest in the book.
    std::vector<Trade> submit(Order incoming);

    // Cancel a resting order by id. Returns true if it was found and removed.
    bool cancel(OrderId id);

    // Best prices, or std::nullopt when that side of the book is empty.
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // --- Read helpers, mostly for tests / introspection ---

    // Is this order currently resting in the book?
    bool contains(OrderId id) const;

    // Total resting quantity at a given price on a given side (0 if none).
    Quantity quantity_at(OrderSide side, Price price) const;

private:
    // asks: lowest price first
    std::map<Price, std::list<Order>> asks_;
    // bids: highest price first
    std::map<Price, std::list<Order>, std::greater<Price>> bids_;

    // O(1) cancel lookup.
    std::unordered_map<OrderId, Locator> index_;

    // Match one price level (a FIFO list) against the incoming order, emitting
    // trades at level_price (the maker's resting price). Fully-filled resting
    // orders are erased from the list and from index_.
    void matchAtLevel(Order& incoming, std::list<Order>& resting,
                      Price level_price, std::vector<Trade>& trades);

    // Rest whatever quantity remains of `incoming` and index it.
    void rest(const Order& incoming);

    // FOK pre-check: can the whole order fill against currently crossing
    // liquidity? Counts by *quantity*, mutating nothing.
    bool canFullyFill(const Order& incoming) const;

    // Does an incoming order cross a resting price level?
    //   - MARKET crosses any level.
    //   - LIMIT BUY  crosses when incoming.price >= level_price.
    //   - LIMIT SELL crosses when incoming.price <= level_price.
    static bool crosses(const Order& incoming, Price level_price);

    // Walk the opposite book from the best price, matching until the incoming
    // order is exhausted or the spread no longer crosses. Templated only so it
    // works for both comparator types (bids_ uses std::greater).
    template <typename OppositeBook>
    void matchAgainst(Order& incoming, OppositeBook& opposite,
                      std::vector<Trade>& trades)
    {
        while (incoming.quantity > 0 && !opposite.empty())
        {
            auto best_it = opposite.begin();
            const Price level_price = best_it->first;

            if (!crosses(incoming, level_price))
                break; // best price no longer crosses -> done matching

            std::list<Order>& level = best_it->second;
            matchAtLevel(incoming, level, level_price, trades);

            if (level.empty())
                opposite.erase(best_it); // price level emptied -> drop it
        }
    }
};
