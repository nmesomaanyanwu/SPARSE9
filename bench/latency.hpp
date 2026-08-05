// latency.hpp -- tiny, dependency-free latency-stats helper shared by the
// benchmarks. Collect raw per-operation nanosecond samples, then ask for
// percentiles. Percentiles use nth_element (partial sort) so summarising 10M
// samples costs a few hundred ms, not a full sort.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace bench
{
using Clock = std::chrono::steady_clock;

// Nanoseconds between two steady_clock time points.
inline std::uint64_t ns_between(Clock::time_point a, Clock::time_point b)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Measure the raw cost of a single Clock::now() pair so a reader can see how
// much of a sub-microsecond figure is the measurement itself. Returns median ns.
inline std::uint64_t measure_clock_overhead_ns(int iters = 200000)
{
    std::vector<std::uint64_t> s;
    s.reserve(iters);
    for (int i = 0; i < iters; ++i)
    {
        auto a = Clock::now();
        auto b = Clock::now();
        s.push_back(ns_between(a, b));
    }
    auto mid = s.begin() + s.size() / 2;
    std::nth_element(s.begin(), mid, s.end());
    return *mid;
}

// A collection of nanosecond samples you can query for percentiles.
struct Samples
{
    std::vector<std::uint32_t> ns; // one entry per measured operation

    void reserve(std::size_t n) { ns.reserve(n); }
    void add(std::uint64_t v)
    {
        // Clamp to uint32 range (~4.29 s); real samples are far below this.
        ns.push_back(v > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                       : static_cast<std::uint32_t>(v));
    }

    std::size_t count() const { return ns.size(); }

    // p in [0,1]. Uses nth_element, so it reorders the buffer -- fine here
    // because we only read stats once at the end.
    std::uint32_t percentile(double p)
    {
        if (ns.empty()) return 0;
        std::size_t idx = static_cast<std::size_t>(p * (ns.size() - 1));
        auto it = ns.begin() + idx;
        std::nth_element(ns.begin(), it, ns.end());
        return *it;
    }

    std::uint32_t min() { return percentile(0.0); }
    std::uint32_t max() { return percentile(1.0); }
};

// Print a labelled percentile block. clock_overhead_ns is informational.
inline void report(const char* title, Samples& s, double seconds_elapsed,
                   std::uint64_t clock_overhead_ns)
{
    std::printf("\n=== %s ===\n", title);
    std::printf("  operations   : %zu\n", s.count());
    std::printf("  wall time    : %.3f s\n", seconds_elapsed);
    if (seconds_elapsed > 0)
        std::printf("  throughput   : %.0f ops/sec\n",
                    s.count() / seconds_elapsed);
    std::printf("  latency (ns, includes ~%llu ns clock overhead):\n",
                (unsigned long long)clock_overhead_ns);
    std::printf("    min   : %u\n", s.min());
    std::printf("    p50   : %u\n", s.percentile(0.50));
    std::printf("    p90   : %u\n", s.percentile(0.90));
    std::printf("    p99   : %u\n", s.percentile(0.99));
    std::printf("    p99.9 : %u\n", s.percentile(0.999));
    std::printf("    max   : %u\n", s.max());
    std::printf("  latency (us):\n");
    std::printf("    p50   : %.3f\n", s.percentile(0.50) / 1000.0);
    std::printf("    p99   : %.3f\n", s.percentile(0.99) / 1000.0);
    std::printf("    p99.9 : %.3f\n", s.percentile(0.999) / 1000.0);
}
} // namespace bench
