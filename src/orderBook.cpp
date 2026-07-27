// orderBook.cpp -- OrderBook implementation
#include "order_book.hpp"

#include <algorithm> // std::min
#include <iterator>  // std::prev

// ---------------------------------------------------------------------------
// crosses: does the incoming order cross a resting level at level_price?
// ---------------------------------------------------------------------------
bool OrderBook::crosses(const Order& incoming, Price level_price)
{
    if (incoming.type == OrderType::MARKET)
        return true; // a market order crosses any price

    if (incoming.side == OrderSide::BUY)
        return incoming.price >= level_price; // buy fills any ask at or below its limit
    else
        return incoming.price <= level_price; // sell fills any bid at or above its limit
}

// ---------------------------------------------------------------------------
// matchAtLevel: FIFO-match the incoming order against one price level.
// Trades print at level_price -- the resting (maker) price, never the taker's.
// ---------------------------------------------------------------------------
void OrderBook::matchAtLevel(Order& incoming, std::list<Order>& resting,
                             Price level_price, std::vector<Trade>& trades)
{
    auto it = resting.begin();
    while (it != resting.end() && incoming.quantity > 0)
    {
        Order& maker = *it;
        const Quantity fill = std::min(incoming.quantity, maker.quantity);

        // Buyer/seller ids depend on which side is the aggressor.
        Trade trade = (incoming.side == OrderSide::BUY)
            ? Trade::create(incoming.order_id, maker.order_id, level_price, fill)
            : Trade::create(maker.order_id, incoming.order_id, level_price, fill);
        trades.push_back(trade);

        incoming.quantity -= fill;
        maker.quantity    -= fill;

        if (maker.quantity == 0)
        {
            index_.erase(maker.order_id); // resting order fully consumed
            it = resting.erase(it);       // erase returns the next iterator
        }
        else
        {
            // Maker only partially filled -> incoming must be exhausted, so the
            // loop condition (incoming.quantity > 0) will stop us next check.
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// rest: park the remaining quantity of `incoming` in the book and index it.
// ---------------------------------------------------------------------------
void OrderBook::rest(const Order& incoming)
{
    if (incoming.side == OrderSide::BUY)
    {
        std::list<Order>& level = bids_[incoming.price];
        level.push_back(incoming);
        index_[incoming.order_id] =
            Locator::create(OrderSide::BUY, incoming.price, std::prev(level.end()));
    }
    else
    {
        std::list<Order>& level = asks_[incoming.price];
        level.push_back(incoming);
        index_[incoming.order_id] =
            Locator::create(OrderSide::SELL, incoming.price, std::prev(level.end()));
    }
}

// ---------------------------------------------------------------------------
// canFullyFill: FOK pre-check. Sum crossing liquidity by QUANTITY (not
// notional) and decide whether the whole incoming order could fill. No mutation.
// ---------------------------------------------------------------------------
bool OrderBook::canFullyFill(const Order& incoming) const
{
    Quantity needed = incoming.quantity;

    if (incoming.side == OrderSide::BUY)
    {
        for (const auto& [price, level] : asks_)
        {
            if (!crosses(incoming, price))
                break; // asks are ascending; once one is too dear, all are
            for (const Order& maker : level)
            {
                needed -= maker.quantity;
                if (needed <= 0)
                    return true;
            }
        }
    }
    else
    {
        for (const auto& [price, level] : bids_)
        {
            if (!crosses(incoming, price))
                break; // bids are descending; once one is too low, all are
            for (const Order& maker : level)
            {
                needed -= maker.quantity;
                if (needed <= 0)
                    return true;
            }
        }
    }

    return needed <= 0;
}

// ---------------------------------------------------------------------------
// submit: the heartbeat.
//   1. FOK -> pre-check; if it can't fully fill, touch nothing.
//   2. Match against the opposite side (best price first, FIFO within a level).
//   3. Decide the fate of any remainder based on order type + time-in-force.
// ---------------------------------------------------------------------------
std::vector<Trade> OrderBook::submit(Order incoming)
{
    std::vector<Trade> trades;

    // 1. Fill-Or-Kill: all-or-nothing. Verify before mutating anything.
    if (incoming.time_in_force == TimeInForce::FOK && !canFullyFill(incoming))
        return trades; // empty -> nothing happened

    // 2. Cross the spread.
    if (incoming.side == OrderSide::BUY)
        matchAgainst(incoming, asks_, trades);
    else
        matchAgainst(incoming, bids_, trades);

    // 3. What happens to the unfilled remainder?
    if (incoming.quantity > 0)
    {
        const bool resting_allowed =
            incoming.type == OrderType::LIMIT &&
            (incoming.time_in_force == TimeInForce::GTC ||
             incoming.time_in_force == TimeInForce::Day);
        // MARKET never rests; IOC and FOK cancel any remainder.
        if (resting_allowed)
            rest(incoming);
    }

    return trades;
}

// ---------------------------------------------------------------------------
// cancel: O(1) via the index. Removes the node and drops an emptied level.
// ---------------------------------------------------------------------------
bool OrderBook::cancel(OrderId id)
{
    auto found = index_.find(id);
    if (found == index_.end())
        return false;

    const Locator& loc = found->second;

    if (loc.side == OrderSide::BUY)
    {
        std::list<Order>& level = bids_.at(loc.price);
        level.erase(loc.order_iterator);
        if (level.empty())
            bids_.erase(loc.price);
    }
    else
    {
        std::list<Order>& level = asks_.at(loc.price);
        level.erase(loc.order_iterator);
        if (level.empty())
            asks_.erase(loc.price);
    }

    index_.erase(found);
    return true;
}

// ---------------------------------------------------------------------------
// best_bid / best_ask
// ---------------------------------------------------------------------------
std::optional<Price> OrderBook::best_bid() const
{
    if (bids_.empty())
        return std::nullopt;
    return bids_.begin()->first; // descending -> highest bid
}

std::optional<Price> OrderBook::best_ask() const
{
    if (asks_.empty())
        return std::nullopt;
    return asks_.begin()->first; // ascending -> lowest ask
}

// ---------------------------------------------------------------------------
// Read helpers
// ---------------------------------------------------------------------------
bool OrderBook::contains(OrderId id) const
{
    return index_.find(id) != index_.end();
}

Quantity OrderBook::quantity_at(OrderSide side, Price price) const
{
    Quantity total = 0;
    if (side == OrderSide::BUY)
    {
        auto it = bids_.find(price);
        if (it != bids_.end())
            for (const Order& o : it->second)
                total += o.quantity;
    }
    else
    {
        auto it = asks_.find(price);
        if (it != asks_.end())
            for (const Order& o : it->second)
                total += o.quantity;
    }
    return total;
}
