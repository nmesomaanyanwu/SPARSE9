// bench_cancel.cpp -- isolates the cost of a single cancel as the book widens.
//
// Cancellation is the operation whose complexity the README makes a claim about,
// so it gets its own measurement rather than being averaged into a mixed
// workload.
//
// Method: build a book `levels` price levels wide with `per_level` resting
// orders at each, then cancel HALF the orders at every level in random order.
// Cancelling only half means no level ever empties, which keeps `map::erase`
// out of the measurement entirely -- what is left is the pure cancel path:
//
//     index_.find(id)            hash lookup
//     loc.level->erase(iter)     list unlink
//     index_.erase(found)        hash erase
//
// Randomised cancel order is deliberate: sequential cancels would be
// prefetch-friendly and flatter the result.
//
//   ./bench_cancel            # default sweep
//
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "order_book.hpp"

namespace
{
Order limit_buy(OrderId id, Price price, Quantity qty)
{
    return Order{id, OrderType::LIMIT, OrderSide::BUY, price, qty,
                 TimeInForce::GTC};
}

double bench_one(int levels, int per_level, unsigned seed)
{
    OrderBook book;
    std::vector<OrderId> victims;
    OrderId id = 1;

    for (int level = 0; level < levels; ++level)
        for (int k = 0; k < per_level; ++k)
        {
            book.submit(limit_buy(id, 1000 - level, 10));
            if (k % 2 == 0) victims.push_back(id); // cancel every other one
            ++id;
        }

    std::mt19937 rng(seed);
    std::shuffle(victims.begin(), victims.end(), rng);

    const auto t0 = std::chrono::steady_clock::now();
    for (OrderId victim : victims) book.cancel(victim);
    const auto t1 = std::chrono::steady_clock::now();

    return std::chrono::duration<double, std::nano>(t1 - t0).count() /
           static_cast<double>(victims.size());
}
} // namespace

int main()
{
    constexpr int kPerLevel = 16;
    constexpr int kReps = 3;

    std::printf("cancel() cost vs book width\n");
    std::printf("  %d orders per level, half cancelled, randomised order\n",
                kPerLevel);
    std::printf("  no level ever empties -> map::erase excluded\n\n");
    std::printf("%12s %14s\n", "price levels", "ns/cancel");

    for (int levels : {16, 64, 256, 1024, 4096, 16384})
    {
        double best = 1e18;
        for (int rep = 0; rep < kReps; ++rep)
            best = std::min(best, bench_one(levels, kPerLevel, 42));
        std::printf("%12d %14.1f\n", levels, best);
    }

    std::printf(
        "\nNote: any growth here is memory-hierarchy cost (cache misses as the\n"
        "working set grows), not algorithmic -- the operation count per cancel\n"
        "is constant. Big-O counts operations, not nanoseconds.\n");
    return 0;
}
