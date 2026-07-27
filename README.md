# Matching Engine — Phase 1 (correctness-first baseline)

A single-threaded limit-order-book matching engine built on `std::map` +
`std::list`. This is the **deliberately obvious, clear, slightly-slow**
implementation. It is optimized for one thing: **correctness**. It is the
baseline you keep forever — the thing a future fast version gets benchmarked
and differential-tested against.

---

## What it does

- Full **price-time priority**: better price first, oldest-first (FIFO) within a
  price level.
- **Maker-price execution**: a trade prints at the resting (maker) order's
  price, never the aggressor's limit.
- Order types: **LIMIT** and **MARKET**.
- Time-in-force: **GTC**, **Day**, **IOC**, **FOK**.
- `O(1)` **cancel** via an `OrderId → Locator` index.
- Deterministic: the same input sequence always produces the same trades.

### Order-type / time-in-force semantics

The type and TIF are just instructions about what to do with the **remainder**
after matching:

| Kind          | Crosses at          | Unfilled remainder        |
|---------------|---------------------|---------------------------|
| LIMIT + GTC   | its limit price     | rests in the book         |
| LIMIT + Day   | its limit price     | rests in the book¹        |
| LIMIT + IOC   | its limit price     | cancelled (never rests)   |
| LIMIT + FOK   | its limit price     | all-or-nothing²           |
| MARKET        | any price           | dropped (never rests)     |

¹ `Day` behaves like `GTC` in Phase 1 — there is no session clock yet.
² **FOK** runs a pre-check first: it counts crossing liquidity by *quantity*;
if the whole order can't fill, it touches nothing and returns no trades.

---

## Layout

```
include/
  types.hpp          Price / Quantity / OrderId / TradeId aliases (int64 ticks)
  order_side.hpp     enum class OrderSide { BUY, SELL }
  order_type.hpp     enum class OrderType { LIMIT, MARKET }
  time_in_force.hpp  enum class TimeInForce { GTC, IOC, Day, FOK }
  order.hpp          struct Order
  trade.hpp          struct Trade  (+ Trade::create factory)
  locator.hpp        struct Locator { side, price, list<Order>::iterator }
  order_book.hpp     class OrderBook — the public API
src/
  orderBook.cpp      the engine implementation
  main.cpp           a small hand-driven demo
test/
  test_order_book.cpp  dependency-free edge-case suite (23 tests)
```

### How the book is stored

```
asks_  : std::map<Price, std::list<Order>>                 ascending  → begin() = best (lowest)  ask
bids_  : std::map<Price, std::list<Order>, std::greater<>> descending → begin() = best (highest) bid
index_ : std::unordered_map<OrderId, Locator>              O(1) cancel lookup
```

**Ownership.** The book owns every resting `Order`; it lives inside the
`std::list` at its price level. The `Locator` in `index_` holds a
`std::list<Order>::iterator` to that node — valid because a list iterator
survives insertion and erasure of *other* nodes in the same list. That's the
whole trick that makes `cancel` `O(1)` instead of `O(n)`.

---

## Public API

```cpp
std::vector<Trade>   submit(Order incoming);   // match, then rest/cancel/drop the remainder
bool                 cancel(OrderId id);       // remove a resting order; false if not found
std::optional<Price> best_bid() const;         // nullopt if the bid side is empty
std::optional<Price> best_ask() const;         // nullopt if the ask side is empty

// introspection (handy for tests / debugging)
bool     contains(OrderId id) const;
Quantity quantity_at(OrderSide side, Price price) const;
```

`submit` takes the order **by value** on purpose: matching mutates its
remaining quantity, and any leftover may rest in the book.

---

## Requirements

- A C++20 compiler (`clang++` or `g++`)
- CMake ≥ 3.20

---

## Running the program (the demo)

The demo (`src/main.cpp`) rests a couple of orders, fires an aggressive buy that
sweeps the book, then cancels the leftover — printing the trades and top-of-book
at each step.

**Step 1 — configure the build** (once, or after editing `CMakeLists.txt`):

```bash
cmake -S . -B build
```

**Step 2 — compile everything:**

```bash
cmake --build build
```

**Step 3 — run the demo:**

```bash
./build/matching_engine
```

Expected output:

```
Resting two asks: 10@101 (id 1), 5@102 (id 2)
  (no trades)
  (no trades)
  book: best_bid=-  best_ask=101

Aggressive buy 12@102 (id 3) -- sweeps 101 then part of 102:
  TRADE #1: buy 3 x sell 1  10 @ 101
  TRADE #2: buy 3 x sell 2  2 @ 102
  book: best_bid=-  best_ask=102

Cancel remaining ask id 2:
  cancel returned true
  book: best_bid=-  best_ask=-
```

---

## Running the tests

The tests live in their own folder:

```
test/
  test_order_book.cpp   dependency-free edge-case suite (23 tests)
```

They need no external framework (no GoogleTest, no Catch2) — just a compiler.
There are two ways to run them.

### Option A — via CMake + CTest (recommended)

After `cmake --build build` (see above), run:

```bash
ctest --test-dir build --output-on-failure
```

Expected:

```
100% tests passed, 0 tests failed out of 1
```

To see the per-check summary the test binary itself prints, run it directly:

```bash
./build/order_book_tests
```

### Option B — one compiler command (no CMake)

Compile the engine and the test file together and run the result:

```bash
c++ -std=c++20 -Wall -Wextra -Iinclude src/orderBook.cpp test/test_order_book.cpp -o build/tests
./build/tests
```

Either way, the suite prints a summary and exits non-zero if anything fails
(so it works in CI):

```
ALL PASSED: 109/109 checks passed across 23 tests.
```

### Adding your own test

Open `test/test_order_book.cpp` and add a block using the `TEST(...)` macro and
`CHECK(...)` assertions — it auto-registers, no wiring needed:

```cpp
TEST(my_new_case)
{
    OrderBook book;
    book.submit(sell(1, 100, 5));
    auto trades = book.submit(buy(2, 100, 5));
    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 100);
}
```

Rebuild and rerun with either option above.

---

## Using it in code

```cpp
#include "order.hpp"
#include "order_book.hpp"

OrderBook book;

// Rest a sell (maker) at price 100 for 10 units.
book.submit(Order{/*id*/1, OrderType::LIMIT, OrderSide::SELL, /*price*/100, /*qty*/10, TimeInForce::GTC});

// Aggressive buy willing to pay up to 110 for 4 units.
// Executes 4 @ 100 (the maker's price); 6 remain resting as an ask.
std::vector<Trade> trades =
    book.submit(Order{2, OrderType::LIMIT, OrderSide::BUY, 110, 4, TimeInForce::GTC});

for (const Trade& t : trades)
    // t.trade_id, t.buy_order_id, t.sell_order_id, t.price, t.quantity
    ;

book.cancel(1);            // remove the resting remainder
auto ask = book.best_ask(); // std::optional<Price>
```

---

## Edge cases covered by the suite

Each test is a small piece of documentation for a rule:

- empty book (rests, no trades; empty tops)
- non-crossing limit rests instead of trading
- exact fill; incoming larger than resting; incoming smaller than resting
- **maker-price** rule (trade at resting price, not aggressor's limit)
- price-time priority (best price first, FIFO within a level)
- a single order **sweeping three price levels**
- cancel: plain, unknown-id, **partially-filled remainder**, one-of-many-at-a-level
- **IOC**: fills then cancels remainder; no-liquidity no-op
- **MARKET**: takes best, never rests, drops overflow
- **FOK**: fully fillable executes; unfillable is a no-op; respects the limit
  price; exact boundary executes
- sell-aggressor mirror of the buy path
- deterministic replay (same input → same trades)

---

## Interview story

> A correct matching engine with full order-type semantics — limit, market,
> IOC, FOK — with price-time priority and maker-price execution, verified
> against a suite of edge-case tests. Now I make it fast.
