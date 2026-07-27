// test_order_book.cpp
//
// A dependency-free test suite for the Phase 1 matching engine. Each test is a
// small piece of documentation for a semantic rule. Run it and it prints a
// PASS/FAIL summary; the process exit code is non-zero if anything failed.
//
//   c++ -std=c++20 -Iinclude src/orderBook.cpp test/test_order_book.cpp -o build/tests
//   ./build/tests

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"

// --------------------------- tiny test framework ---------------------------
namespace
{
int g_checks = 0;
int g_failures = 0;
std::string g_current_test;

void check(bool cond, const std::string& what, int line)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::cerr << "  FAIL [" << g_current_test << ":" << line << "] " << what
                  << '\n';
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// Order-building helpers so tests read like intent, not boilerplate.
Order limit(OrderId id, OrderSide side, Price price, Quantity qty,
            TimeInForce tif = TimeInForce::GTC)
{
    return Order{id, OrderType::LIMIT, side, price, qty, tif};
}

Order market(OrderId id, OrderSide side, Quantity qty,
             TimeInForce tif = TimeInForce::IOC)
{
    // Market orders carry no meaningful limit price; 0 is a placeholder.
    return Order{id, OrderType::MARKET, side, 0, qty, tif};
}

Order buy(OrderId id, Price price, Quantity qty, TimeInForce tif = TimeInForce::GTC)
{
    return limit(id, OrderSide::BUY, price, qty, tif);
}
Order sell(OrderId id, Price price, Quantity qty, TimeInForce tif = TimeInForce::GTC)
{
    return limit(id, OrderSide::SELL, price, qty, tif);
}

// Test registry.
using TestFn = void (*)();
struct Test { const char* name; TestFn fn; };
std::vector<Test>& registry()
{
    static std::vector<Test> r;
    return r;
}
struct Registrar
{
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};
#define TEST(name)                                                             \
    static void name();                                                       \
    static Registrar registrar_##name(#name, name);                           \
    static void name()

// =========================================================================
//                                 TESTS
// =========================================================================

// An order that finds nothing to match against just rests. No trades.
TEST(empty_book_limit_rests)
{
    OrderBook book;
    auto trades = book.submit(buy(1, 100, 10));
    CHECK(trades.empty());
    CHECK(book.best_bid() == std::optional<Price>(100));
    CHECK(book.best_ask() == std::nullopt);
    CHECK(book.contains(1));
    CHECK(book.quantity_at(OrderSide::BUY, 100) == 10);
}

// Empty book, empty tops.
TEST(empty_book_has_no_tops)
{
    OrderBook book;
    CHECK(book.best_bid() == std::nullopt);
    CHECK(book.best_ask() == std::nullopt);
}

// A limit that does not cross the spread simply rests.
TEST(non_crossing_limit_rests)
{
    OrderBook book;
    book.submit(sell(1, 105, 10));
    auto trades = book.submit(buy(2, 104, 10)); // 104 < 105, no cross
    CHECK(trades.empty());
    CHECK(book.best_bid() == std::optional<Price>(104));
    CHECK(book.best_ask() == std::optional<Price>(105));
}

// Exact fill: both orders vanish, one trade for the full size.
TEST(exact_fill)
{
    OrderBook book;
    book.submit(sell(1, 100, 10));
    auto trades = book.submit(buy(2, 100, 10));
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 10);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].buy_order_id == 2);
    CHECK(trades[0].sell_order_id == 1);
    CHECK(book.best_bid() == std::nullopt);
    CHECK(book.best_ask() == std::nullopt);
    CHECK(!book.contains(1));
}

// Aggressor bigger than the resting order: resting fully fills, remainder rests.
TEST(incoming_larger_leaves_resting_remainder)
{
    OrderBook book;
    book.submit(sell(1, 100, 4));
    auto trades = book.submit(buy(2, 100, 10)); // fills 4, 6 rests as a bid
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 4);
    CHECK(book.best_ask() == std::nullopt);
    CHECK(book.best_bid() == std::optional<Price>(100));
    CHECK(book.quantity_at(OrderSide::BUY, 100) == 6);
    CHECK(book.contains(2));
}

// Aggressor smaller than the resting order: resting order shrinks and stays.
TEST(incoming_smaller_partial_fills_resting)
{
    OrderBook book;
    book.submit(sell(1, 100, 10));
    auto trades = book.submit(buy(2, 100, 3));
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 3);
    CHECK(book.quantity_at(OrderSide::SELL, 100) == 7);
    CHECK(book.contains(1));
    CHECK(!book.contains(2)); // fully-filled aggressor never rests
}

// The maker-price rule: the trade prints at the resting order's price, not the
// aggressor's more generous limit.
TEST(trade_executes_at_maker_price)
{
    OrderBook book;
    book.submit(sell(1, 100, 10));         // maker resting at 100
    auto trades = book.submit(buy(2, 110, 10)); // taker willing to pay up to 110
    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 100); // executes at 100, the maker's price
}

// Price-time priority: best price first, oldest-first within a price.
TEST(price_time_priority)
{
    OrderBook book;
    book.submit(sell(1, 101, 5)); // worse price, older
    book.submit(sell(2, 100, 5)); // best price
    book.submit(sell(3, 100, 5)); // same price, newer than id 2
    // Buyer sweeps: should hit 100/id2, then 100/id3, then 101/id1.
    auto trades = book.submit(buy(4, 101, 15));
    CHECK(trades.size() == 3);
    CHECK(trades[0].sell_order_id == 2); // best price, oldest
    CHECK(trades[0].price == 100);
    CHECK(trades[1].sell_order_id == 3); // best price, next in FIFO
    CHECK(trades[1].price == 100);
    CHECK(trades[2].sell_order_id == 1); // next price level
    CHECK(trades[2].price == 101);
}

// A single order sweeping three price levels.
TEST(sweep_three_levels)
{
    OrderBook book;
    book.submit(sell(1, 100, 5));
    book.submit(sell(2, 101, 5));
    book.submit(sell(3, 102, 5));
    auto trades = book.submit(buy(4, 102, 15)); // exactly clears all three
    CHECK(trades.size() == 3);
    CHECK(trades[0].price == 100);
    CHECK(trades[1].price == 101);
    CHECK(trades[2].price == 102);
    CHECK(book.best_ask() == std::nullopt);
    CHECK(book.best_bid() == std::nullopt); // aggressor fully filled, nothing rests
}

// Cancel a plain resting order.
TEST(cancel_resting_order)
{
    OrderBook book;
    book.submit(buy(1, 100, 10));
    CHECK(book.cancel(1));
    CHECK(!book.contains(1));
    CHECK(book.best_bid() == std::nullopt);
}

// Cancelling an unknown id is a no-op that reports false.
TEST(cancel_unknown_returns_false)
{
    OrderBook book;
    CHECK(!book.cancel(999));
    book.submit(buy(1, 100, 10));
    CHECK(!book.cancel(2));
}

// Cancel the remainder of a partially-filled order.
TEST(cancel_partially_filled_remainder)
{
    OrderBook book;
    book.submit(sell(1, 100, 4));
    book.submit(buy(2, 100, 10)); // fills 4, 6 rests as bid id 2
    CHECK(book.quantity_at(OrderSide::BUY, 100) == 6);
    CHECK(book.cancel(2));
    CHECK(!book.contains(2));
    CHECK(book.best_bid() == std::nullopt);
}

// Cancelling one order at a shared price level leaves the others intact.
TEST(cancel_one_of_many_at_level)
{
    OrderBook book;
    book.submit(buy(1, 100, 5));
    book.submit(buy(2, 100, 7));
    CHECK(book.cancel(1));
    CHECK(!book.contains(1));
    CHECK(book.contains(2));
    CHECK(book.quantity_at(OrderSide::BUY, 100) == 7);
    CHECK(book.best_bid() == std::optional<Price>(100));
}

// IOC fills what it can right now and cancels the rest -- it never rests.
TEST(ioc_fills_then_cancels_remainder)
{
    OrderBook book;
    book.submit(sell(1, 100, 4));
    auto trades = book.submit(buy(2, 100, 10, TimeInForce::IOC));
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 4);
    CHECK(!book.contains(2));         // remainder cancelled, not rested
    CHECK(book.best_bid() == std::nullopt);
}

// IOC with nothing to match against produces no trades and rests nothing.
TEST(ioc_no_liquidity_is_noop)
{
    OrderBook book;
    auto trades = book.submit(buy(1, 100, 10, TimeInForce::IOC));
    CHECK(trades.empty());
    CHECK(!book.contains(1));
    CHECK(book.best_bid() == std::nullopt);
}

// Market order takes the best available liquidity and never rests.
TEST(market_order_takes_best_and_never_rests)
{
    OrderBook book;
    book.submit(sell(1, 100, 5));
    book.submit(sell(2, 101, 5));
    auto trades = book.submit(market(3, OrderSide::BUY, 8));
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 100); // best first
    CHECK(trades[1].price == 101);
    CHECK(trades[1].quantity == 3);
    CHECK(!book.contains(3));       // markets never rest
    CHECK(book.quantity_at(OrderSide::SELL, 101) == 2);
}

// Market order with a remainder beyond available liquidity just drops it.
TEST(market_order_drops_unfilled_remainder)
{
    OrderBook book;
    book.submit(sell(1, 100, 5));
    auto trades = book.submit(market(2, OrderSide::BUY, 12));
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 5);
    CHECK(book.best_ask() == std::nullopt);
    CHECK(book.best_bid() == std::nullopt); // nothing rested
}

// FOK that CAN fully fill executes completely.
TEST(fok_fully_fillable_executes)
{
    OrderBook book;
    book.submit(sell(1, 100, 6));
    book.submit(sell(2, 101, 6));
    auto trades = book.submit(buy(3, 101, 10, TimeInForce::FOK));
    CHECK(trades.size() == 2);
    CHECK(trades[0].quantity == 6);
    CHECK(trades[1].quantity == 4);
    CHECK(book.quantity_at(OrderSide::SELL, 101) == 2);
    CHECK(!book.contains(3));
}

// FOK that CANNOT fully fill touches nothing at all.
TEST(fok_unfillable_is_noop)
{
    OrderBook book;
    book.submit(sell(1, 100, 6)); // only 6 available at/under limit
    auto trades = book.submit(buy(2, 100, 10, TimeInForce::FOK));
    CHECK(trades.empty());               // no partial fills
    CHECK(book.quantity_at(OrderSide::SELL, 100) == 6); // resting order untouched
    CHECK(!book.contains(2));
    CHECK(book.best_ask() == std::optional<Price>(100));
}

// FOK where liquidity exists but sits beyond the limit price -> unfillable.
TEST(fok_respects_limit_price)
{
    OrderBook book;
    book.submit(sell(1, 100, 5));
    book.submit(sell(2, 105, 10)); // exists, but above the buyer's 100 limit
    auto trades = book.submit(buy(3, 100, 10, TimeInForce::FOK));
    CHECK(trades.empty()); // only 5 available at/under 100; 10 needed
    CHECK(book.quantity_at(OrderSide::SELL, 100) == 5);
    CHECK(book.quantity_at(OrderSide::SELL, 105) == 10);
}

// FOK at the exact boundary (available == needed) executes.
TEST(fok_exact_boundary_executes)
{
    OrderBook book;
    book.submit(sell(1, 100, 4));
    book.submit(sell(2, 100, 6)); // total 10 at 100
    auto trades = book.submit(buy(3, 100, 10, TimeInForce::FOK));
    CHECK(!trades.empty());
    Quantity filled = 0;
    for (const auto& t : trades) filled += t.quantity;
    CHECK(filled == 10);
    CHECK(book.best_ask() == std::nullopt);
}

// A sell aggressor mirrors the buy logic (best bid first, maker price).
TEST(sell_aggressor_hits_bids)
{
    OrderBook book;
    book.submit(buy(1, 100, 5));
    book.submit(buy(2, 101, 5)); // best bid
    auto trades = book.submit(sell(3, 100, 8));
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 101); // best bid first
    CHECK(trades[0].buy_order_id == 2);
    CHECK(trades[0].sell_order_id == 3);
    CHECK(trades[1].price == 100);
    CHECK(book.quantity_at(OrderSide::BUY, 100) == 2);
}

// Determinism: identical input sequences yield identical trade sequences.
TEST(deterministic_replay)
{
    auto run = []() {
        OrderBook book;
        std::vector<Trade> all;
        auto collect = [&](std::vector<Trade> t) {
            for (auto& x : t) all.push_back(x);
        };
        collect(book.submit(sell(1, 101, 5)));
        collect(book.submit(sell(2, 100, 5)));
        collect(book.submit(buy(3, 101, 7)));
        collect(book.submit(buy(4, 99, 3)));
        collect(book.submit(sell(5, 99, 10)));
        return all;
    };
    auto a = run();
    auto b = run();
    CHECK(a.size() == b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        CHECK(a[i].buy_order_id == b[i].buy_order_id);
        CHECK(a[i].sell_order_id == b[i].sell_order_id);
        CHECK(a[i].price == b[i].price);
        CHECK(a[i].quantity == b[i].quantity);
    }
}

} // namespace

int main()
{
    for (const auto& t : registry())
    {
        g_current_test = t.name;
        t.fn();
    }
    std::cout << "\n" << (g_failures == 0 ? "ALL PASSED" : "FAILURES PRESENT")
              << ": " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed across " << registry().size() << " tests.\n";
    return g_failures == 0 ? 0 : 1;
}
