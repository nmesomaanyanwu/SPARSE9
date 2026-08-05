// bench_core.cpp -- measures the latency of the matching core in isolation.
//
// This is NOT an end-to-end order-to-ack number: there is no socket, no queue,
// no second thread. It times a single call into OrderBook (submit or cancel) on
// the thread that owns the book. It is the floor -- the cost of the matching
// work itself -- and every other benchmark in this repo is this plus overhead.
//
// Workload: a steady-state book held near a target depth.
//   * If the book is below target depth, add a resting (non-crossing) limit.
//   * Otherwise, 50/50 between a marketable order (crosses, generates trades)
//     and a cancel of a previously-added order.
// Prices random-walk around a mid, so liquidity is spread across many levels
// rather than piling onto one. Everything is driven by a seeded PRNG, so a run
// is fully reproducible.
//
// Usage: bench_core [num_ops] [target_depth] [seed]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "latency.hpp"

namespace
{
Order make(OrderId id, OrderSide side, OrderType type, Price price, Quantity qty,
           TimeInForce tif = TimeInForce::GTC)
{
    return Order{id, type, side, price, qty, tif};
}
} // namespace

int main(int argc, char** argv)
{
    const std::size_t num_ops =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000ull;
    const std::size_t target_depth =
        (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000ull;
    const std::uint64_t seed =
        (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 42ull;

    std::printf("bench_core: ops=%zu target_depth=%zu seed=%llu\n", num_ops,
                target_depth, (unsigned long long)seed);

    OrderBook book;
    std::mt19937_64 rng(seed);

    // Live resting order ids we can later cancel. A simple pool: cancel picks a
    // random slot and swap-removes it.
    std::vector<OrderId> live;
    live.reserve(target_depth * 2);

    OrderId next_id = 1;
    const Price mid = 100'000;      // integer ticks
    const Price band = 500;         // prices sit within [1, band] ticks of mid
    std::uniform_int_distribution<Price> off_dist(0, band - 1);
    std::uniform_int_distribution<Quantity> qty_dist(1, 50);
    std::uniform_int_distribution<int> coin(0, 1);

    // Counters so we can describe the realised mix honestly afterwards.
    std::size_t n_add = 0, n_take = 0, n_cancel = 0;
    std::size_t n_trades = 0, n_cancel_hit = 0;

    bench::Samples lat;
    lat.reserve(num_ops);

    const std::uint64_t clock_overhead = bench::measure_clock_overhead_ns();

    auto t_start = bench::Clock::now();

    for (std::size_t i = 0; i < num_ops; ++i)
    {
        if (live.size() < target_depth)
        {
            // ADD a resting limit that does NOT cross: buys below mid, sells
            // above mid. Random side each time keeps both books populated.
            const bool buy = coin(rng);
            const OrderId id = next_id++;
            Price px = buy ? mid - 1 - off_dist(rng)
                           : mid + 1 + off_dist(rng);
            Order o = make(id, buy ? OrderSide::BUY : OrderSide::SELL,
                           OrderType::LIMIT, px, qty_dist(rng));

            auto a = bench::Clock::now();
            auto trades = book.submit(o);
            auto b = bench::Clock::now();
            lat.add(bench::ns_between(a, b));

            n_add++;
            n_trades += trades.size();
            // Only track it as cancellable if it actually rested (no trades
            // means it didn't cross -> it is resting).
            if (trades.empty()) live.push_back(id);
        }
        else if (coin(rng))
        {
            // TAKE: a marketable limit that crosses the spread. Buy priced at
            // the top of the band (>= many asks), sell at the bottom.
            const bool buy = coin(rng);
            const OrderId id = next_id++;
            Price px = buy ? mid + band : mid - band;
            Order o = make(id, buy ? OrderSide::BUY : OrderSide::SELL,
                           OrderType::LIMIT, px, qty_dist(rng),
                           TimeInForce::IOC); // don't let remainder rest

            auto a = bench::Clock::now();
            auto trades = book.submit(o);
            auto b = bench::Clock::now();
            lat.add(bench::ns_between(a, b));

            n_take++;
            n_trades += trades.size();
        }
        else
        {
            // CANCEL a random live id (may already be filled -> miss).
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t slot = pick(rng);
            OrderId id = live[slot];

            auto a = bench::Clock::now();
            bool ok = book.cancel(id);
            auto b = bench::Clock::now();
            lat.add(bench::ns_between(a, b));

            n_cancel++;
            if (ok) n_cancel_hit++;
            live[slot] = live.back();
            live.pop_back();
        }
    }

    auto t_end = bench::Clock::now();
    double seconds = bench::ns_between(t_start, t_end) / 1e9;

    std::printf("\nrealised mix: add=%zu (%.1f%%) take=%zu (%.1f%%) "
                "cancel=%zu (%.1f%%)\n",
                n_add, 100.0 * n_add / num_ops, n_take,
                100.0 * n_take / num_ops, n_cancel,
                100.0 * n_cancel / num_ops);
    std::printf("trades generated: %zu | cancel hit-rate: %.1f%% | "
                "final resting depth (tracked): %zu\n",
                n_trades, n_cancel ? 100.0 * n_cancel_hit / n_cancel : 0.0,
                live.size());

    bench::report("core submit/cancel latency", lat, seconds, clock_overhead);
    return 0;
}
