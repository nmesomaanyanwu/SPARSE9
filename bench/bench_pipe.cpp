// bench_pipe.cpp -- order-to-ack latency and throughput across the in-process
// single-writer boundary: a producer thread and one matching thread joined by
// the bounded MPSC queue. No sockets yet. This isolates the cost the queue and
// the thread hand-off add on top of the bare matching core (bench_core).
//
// Two measurements:
//   1. Latency: one order in flight. The producer stamps ingress, enqueues,
//      and spins until the matching thread has processed it and published an
//      ack token. ingress->ack is the sample. This is the honest order-to-ack
//      round trip minus the network.
//   2. Throughput: the producer floods the queue; we measure how fast the
//      single matching thread drains N operations.
//
// Usage: bench_pipe [num_ops] [target_depth] [seed]

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "mpsc_queue.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "latency.hpp"
#include "workload.hpp"

namespace
{
// A single request travelling producer -> matching thread. POD so it lives in
// the queue by value with no allocation.
struct Request
{
    enum Kind : std::uint8_t { Submit, Cancel } kind;
    Order order;
    OrderId cancel_id;
    std::uint64_t token; // producer waits for this to appear on the ack side
};

// The matching thread publishes the highest token it has processed here.
std::atomic<std::uint64_t> g_ack_token{0};
std::atomic<bool> g_running{true};

inline void cpu_relax()
{
#if defined(__aarch64__) || defined(__arm64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#endif
}

// The single writer: it alone touches the book.
void matching_thread(MpscQueue<Request>* q, OrderBook* book)
{
    Request r;
    while (g_running.load(std::memory_order_relaxed))
    {
        if (q->try_dequeue(r))
        {
            if (r.kind == Request::Cancel)
                book->cancel(r.cancel_id);
            else
                book->submit(r.order);
            g_ack_token.store(r.token, std::memory_order_release);
        }
        else
        {
            cpu_relax();
        }
    }
}

Request to_request(const Workload::Op& op, std::uint64_t token)
{
    if (op.kind == Workload::Op::Cancel)
        return Request{Request::Cancel, {}, op.cancel_id, token};
    return Request{Request::Submit, op.order, 0, token};
}
} // namespace

int main(int argc, char** argv)
{
    const std::size_t num_ops =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 5'000'000ull;
    const std::size_t target_depth =
        (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000ull;
    const std::uint64_t seed =
        (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 42ull;

    std::printf("bench_pipe: ops=%zu target_depth=%zu seed=%llu\n", num_ops,
                target_depth, (unsigned long long)seed);

    OrderBook book;
    MpscQueue<Request> queue(1 << 16); // 65,536 slots

    std::thread matcher(matching_thread, &queue, &book);

    auto enqueue_blocking = [&](const Request& r) {
        while (!queue.enqueue(r)) cpu_relax();
    };
    auto wait_token = [&](std::uint64_t tok) {
        while (g_ack_token.load(std::memory_order_acquire) < tok) cpu_relax();
    };

    std::uint64_t token = 0;
    const std::uint64_t clock_overhead = bench::measure_clock_overhead_ns();

    // --- Warm up: fill the book to target depth (round-trip each) ---
    {
        Workload warm(target_depth, seed);
        while (warm.pool_size() < target_depth)
        {
            Workload::Op op = warm.next();
            ++token;
            enqueue_blocking(to_request(op, token));
            wait_token(token);
            warm.record(op, true);
        }
    }

    // ------------------------------------------------------------------
    // 1. LATENCY: one order in flight, measure ingress -> ack round trip.
    // ------------------------------------------------------------------
    {
        Workload wl(target_depth, seed);
        // fast-forward the generator past the warm-up adds so the measured
        // stream is the steady-state mix, matching bench_core's stream shape.
        while (wl.pool_size() < target_depth)
            wl.record(wl.next(), true);

        bench::Samples lat;
        lat.reserve(num_ops);
        auto t0 = bench::Clock::now();
        for (std::size_t i = 0; i < num_ops; ++i)
        {
            Workload::Op op = wl.next();
            ++token;
            Request req = to_request(op, token);
            auto a = bench::Clock::now();
            enqueue_blocking(req);
            wait_token(token);
            auto b = bench::Clock::now();
            lat.add(bench::ns_between(a, b));
            wl.record(op, op.kind == Workload::Op::Add);
        }
        auto t1 = bench::Clock::now();
        bench::report("order-to-ack round trip (in-process, 1 in flight)", lat,
                      bench::ns_between(t0, t1) / 1e9, clock_overhead);
    }

    // ------------------------------------------------------------------
    // 2. THROUGHPUT: flood the queue, measure drain rate.
    // ------------------------------------------------------------------
    {
        Workload wl(target_depth, seed);
        while (wl.pool_size() < target_depth)
            wl.record(wl.next(), true);

        std::uint64_t start_token = token;
        auto t0 = bench::Clock::now();
        for (std::size_t i = 0; i < num_ops; ++i)
        {
            Workload::Op op = wl.next();
            ++token;
            enqueue_blocking(to_request(op, token));
            wl.record(op, op.kind == Workload::Op::Add);
        }
        wait_token(token); // drain
        auto t1 = bench::Clock::now();
        double secs = bench::ns_between(t0, t1) / 1e9;
        std::printf("\n=== saturated throughput (in-process) ===\n");
        std::printf("  operations : %llu\n",
                    (unsigned long long)(token - start_token));
        std::printf("  wall time  : %.3f s\n", secs);
        std::printf("  throughput : %.0f ops/sec\n",
                    (token - start_token) / secs);
    }

    g_running.store(false, std::memory_order_relaxed);
    matcher.join();
    return 0;
}
