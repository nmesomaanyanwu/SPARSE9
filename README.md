# SPARSE9 — Limit Order Book & Matching Engine

A limit order book and matching engine in C++20, with a TCP order-entry gateway
and a single-writer concurrency model. Full price-time priority with LIMIT,
MARKET, IOC and FOK semantics, and O(1) cancellation.

The matching **core** processes an order in a **median of 84 ns** (p99 584 ns).
Wrapped in the full TCP path, **end-to-end order-to-ack** over loopback is a
**median of 14.6 µs** (p99 43.7 µs). Those are two different measurements of two
different things — read [Benchmarks](#benchmarks) before comparing either to
anything, because the conditions (macOS, loopback, no core isolation) matter a
lot.

Design notes and write-ups: [The Latency Log](https://nmesomaanyanwu.hashnode.dev)

---

## Contents

- [Benchmarks](#benchmarks)
- [Architecture](#architecture)
- [Design decisions](#design-decisions)
- [Matching semantics](#matching-semantics)
- [Correctness](#correctness)
- [Ongoing work](#ongoing-work)
- [Build and run](#build-and-run)
- [Limitations](#limitations)

---

## Benchmarks

There are **three** measurements, each isolating a different layer. They share
one seeded workload generator (`bench/workload.hpp`), so the only thing that
changes between them is the transport.

### 1. Matching core, in-process (`bench_core`)

No queue, no thread hand-off, no socket — a direct call into `OrderBook` on the
thread that owns it. This is the floor.

| Metric | Value |
| --- | --- |
| Orders measured | 10,000,000 |
| Median (p50) | 84 ns |
| p99 | 584 ns |
| p99.9 | 916 ns |
| Throughput (single thread) | 5.1 M orders/sec |

### 2. Order-to-ack across the single-writer boundary, in-process (`bench_pipe`)

Producer thread → bounded MPSC queue → matching thread → ack. Still no socket.
This is the cost the queue and the thread hand-off add on top of the core.

| Metric | Value |
| --- | --- |
| Orders measured | 5,000,000 |
| Median (p50) | 292 ns |
| p99 | 875 ns |
| p99.9 | 1.75 µs |
| Saturated throughput | 6.4 M orders/sec |

### 3. End-to-end order-to-ack over TCP loopback (`bench_tcp`) — the headline

Client `send()` of a length-prefixed order frame → gateway parse → MPSC queue →
matching thread → ack frame → client `recv()`.

| Metric | Value |
| --- | --- |
| Orders measured | 1,000,000 |
| Median (p50) | 14.6 µs |
| p99 | 43.7 µs |
| p99.9 | 68.9 µs |
| Throughput, 1 order in flight | 62.6 k orders/sec |
| Throughput, pipelined | 145 k orders/sec |

**What is being measured (measurement 3).** A steady-clock timestamp is taken on
the client immediately before `send()` writes the order frame to the socket, and
again immediately after `recv()` returns the matching ack frame. The reported
figure is the difference: one full round trip, single order in flight.

**Conditions.** Single producer, single connection, one order in flight, steady
state, warm cache. Latencies include ~41 ns of `steady_clock::now()` overhead
per sample (measured and reported by the harness).

| | |
| --- | --- |
| CPU | Apple M5 (10 cores), Mac17,3 |
| Cores pinned / isolated | **None** — macOS gives no `isolcpus`/affinity equivalent; threads float across cores under the normal scheduler |
| OS / kernel | macOS 26.5.1, Darwin 25.5.0 |
| Compiler and flags | Apple clang 21.0.0, C++20, `-O3` (CMake `Release`), `-Wall -Wextra -Werror` |
| Network path | TCP over the **loopback** interface, same host — not a physical NIC |
| Clock source | `std::chrono::steady_clock` (~41 ns/call on this machine) |
| Book state | ~5,000 resting orders, held steady across a ±500-tick price band |
| Order mix | ≈33% resting adds, ≈33% marketable IOC takes, ≈33% cancels, seeded RNG (`seed=42`) |

**Where the time goes.** Stacking the three measurements attributes the latency
cleanly:

```
  matching core .................   84 ns   (measurement 1)
  + MPSC queue + thread hand-off .  292 ns   (measurement 2, cumulative)
  + TCP loopback round trip ...... 14.6 µs   (measurement 3, cumulative)
```

The matching work is ~0.6% of the end-to-end median. **Essentially all** of the
14.6 µs is the socket path: the `send()`/`recv()` syscalls, the loopback network
stack, and the thread wake-ups the hand-off implies. That is the honest story
here — a fast matching core behind an un-tuned, portable, blocking-socket gateway
on a general-purpose OS.

Why is p99 (~44 µs) roughly 3× the median? On this setup it is scheduler jitter:
with no core pinning, the producer, gateway and matching threads are migrated and
occasionally descheduled, and a wake-up that lands after a context switch pays for
it. The max samples (hundreds of µs) line up with that. What I would measure next
to confirm it: sample scheduler/context-switch events and correlate them with the
tail, then re-run with threads pinned (which needs Linux) to see whether the tail
collapses.

**Reproducing.**

```bash
git clone https://github.com/nmesomaanyanwu/SPARSE9.git   # or your local path
cd SPARSE9
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/bench_core 10000000 5000 42   # measurement 1
./build/bench_pipe  5000000 5000 42   # measurement 2
./build/bench_tcp   1000000 5000 42   # measurement 3  (loopback, ~16 s)

./build/bench_cancel                  # cancel cost vs book width (see Ongoing work)
```

Arguments are `[num_ops] [target_depth] [seed]`. Numbers above are from a single
run each; expect small run-to-run variation on an un-isolated machine. A 10 M
end-to-end run reproduces the same median (p50 15.2 µs, p99 47.6 µs) — the table
uses 1 M only because loopback round trips cap throughput at ~60 k/s.

---

## Architecture

```
  TCP clients            gateway thread (1 per conn)        matching thread
 ───────────────  bytes  ──────────────────────────  MPSC  ──────────────────
  producer 1  ──────────▶ read length-prefixed frame ─────▶  owns book memory
  producer 2  ──────────▶ parse, assign order id     ─────▶  submit / cancel
      ...                 push request onto queue            write ack to socket
```

**Order entry.** A TCP gateway speaks a binary, length-prefixed wire protocol
(`include/wire.hpp`): every frame is `[uint32 length][payload]`. Order IDs are
64-bit and producer-partitioned — the top 16 bits are a per-connection producer
id, the low 48 a per-producer counter — so producers mint globally-unique IDs
without coordinating.

**Isolation.** Networking is separated from the matching core by a bounded
multi-producer, single-consumer queue (`include/mpsc_queue.hpp`, Vyukov's bounded
ring). The matching **core** (`OrderBook`) has no knowledge of sockets, framing
or clients, which keeps it directly unit-testable. The matching **thread**
orchestrates: it dequeues, calls into the core, and writes the ack frame back to
the originating socket.

**Single-writer core.** Exactly one thread owns book memory. Producers (gateway
threads) enqueue requests; they never touch the book. This structurally
eliminates four failure modes rather than defending against them at runtime:

| Failure mode | Why it cannot occur |
| --- | --- |
| Torn quantity updates | Only one thread ever writes an order's fields |
| Dangling best-price pointers | Level erase and pointer update happen in one thread, in order |
| Scrambled time priority | Queue order into the matcher *is* arrival order |
| Deadlock | The matching core takes no locks |

---

## Design decisions

The single-writer model was chosen over three alternatives. **These rejections
are reasoned, not measured** — I did not implement the global-lock or
per-level-lock variants and benchmark them; I ruled them out on analysis before
building. That distinction matters, so I am stating it plainly.

**Global lock over the whole book.** Simple and obviously correct, but every
producer serialises on one mutex, so throughput collapses under contention and
tail latency becomes dominated by lock-wait rather than matching work. The
matching work here is ~84 ns, so an uncontended mutex round trip is already the
same order of magnitude, and a contended one dwarfs it.

**Per-price-level locking.** Finer-grained, but a marketable order that sweeps
several levels must hold or hand off locks across levels in price order, which
reintroduces lock-ordering complexity and races on level creation/destruction.
Worse, time priority is no longer guaranteed by arrival order once independent
levels progress concurrently — the property the book exists to provide.

**Lock-free shared book.** Attractive on paper. Rejected as unverifiable at this
scale: correct CAS sequences against ABA hazards, the acquire-release ordering
between price-level and order-node updates, and safe memory reclamation for
erased nodes together produce a design I could not convince myself was correct
and could not test into confidence.

The chosen design gives the same guarantees while leaving the matching core and
its full test suite unchanged — the concurrency and networking layers were added
on top of the correctness-first core without editing a line of it.

---

## Matching semantics

- **Price-time priority** across all order types
- **Maker-price execution** — crossing trades execute at the resting order's price
- **LIMIT**, **MARKET**, **IOC**, **FOK** (× GTC / Day time-in-force)
- **O(1) cancellation** — the `OrderId → Locator` index stores the price
  level's address alongside a stable `std::list` iterator, so a cancel unlinks
  the node with no lookup and no scan. Removing a level that a cancel *empties*
  costs O(log L) in the number of price levels, but that runs once per level's
  lifetime, not once per cancel. See [Ongoing work](#ongoing-work) for the
  measurement.
- **FOK** is pre-checked against crossing liquidity by quantity before any
  mutation, so a kill touches nothing

---

## Correctness

The matching core is covered by **23 tests** in `test/test_order_book.cpp`,
dependency-free and wired into CTest. They cover, among others:

- resting vs. crossing limits, exact / partial / remainder fills
- maker-price execution and FIFO time priority within a level
- multi-level sweeps
- cancel: resting, unknown id, partially-filled remainder, one-of-many at a level
- IOC fill-then-cancel and IOC-with-no-liquidity
- MARKET taking best and never resting; MARKET dropping unfilled remainder
- FOK fully-fillable, unfillable, limit-price-respecting, exact-boundary
- sell aggressor hitting bids; deterministic replay

The concurrency and networking layers were added **without modifying the matching
core**, and this pre-existing suite passes unchanged — the concrete evidence that
the isolation boundary actually holds.

```bash
ctest --test-dir build
```

---

## Ongoing work

Changes made after the first working version, with the reasoning — and, where
the change makes a performance claim, the measurement behind it. Status is
stated per item rather than implied.

### O(1) cancellation — **done, measured**

The original `Locator` stored `{side, price, list_iterator}`. It already held
the order's exact address, but `cancel()` still had to *find the list* in order
to call `erase` on it — an iterator doesn't know which container owns it — so
every cancel paid a `std::map::at` red-black-tree descent, O(log L).

The `Locator` now also stores the level's address directly:

```cpp
struct Locator {
    OrderSide side;
    Price price;                          // only for the empty-level path
    std::list<Order>* level;              // ← removes the tree descent
    std::list<Order>::iterator order_iterator;
};
```

A raw pointer is correct here because it *observes* rather than owns: `std::map`
nodes never relocate, and a level is only erased from the map once it is empty —
at which point no resting order, and therefore no `Locator`, can still reference
it. That invariant is what makes the pointer safe, and it's stated in the header
next to the field.

Measured with `bench_cancel`, which isolates the cancel path by cancelling only
half the orders at each level so no level ever empties:

| price levels | before | after |
| ---: | ---: | ---: |
| 16 | 18.9 ns | **10.4 ns** |
| 64 | 22.5 ns | **13.8 ns** |
| 256 | 36.9 ns | **19.5 ns** |
| 1,024 | 61.6 ns | **28.3 ns** |
| 4,096 | 82.8 ns | **32.9 ns** |
| 16,384 | 161.8 ns | **93.6 ns** |

> **Measurement conditions differ from the rest of this README.** These numbers
> were taken on aarch64 Linux with GCC, not on the Apple M5 / Apple clang setup
> used for the three headline benchmarks. They are included to show the shape of
> the change, not to be compared against the 84 ns figure. Re-run
> `./build/bench_cancel` locally for numbers on your own machine.

Roughly 2× across the range. The residual growth is **not** algorithmic — the
operation count per cancel is constant. It's memory-hierarchy cost: at 16,384
levels the working set is hundreds of thousands of orders, and randomised cancel
order defeats prefetching, so each of the three pointer chases can miss cache.
Big-O counts operations, not nanoseconds.

The 23-test suite passes unchanged.

### RAII for the queue buffer — **done**

`MpscQueue` held its ring in a raw `Cell*` with a matching `delete[]` in the
destructor. It is now a `std::unique_ptr<Cell[]>`, and the destructor is gone.

Zero overhead — `sizeof(std::unique_ptr<Cell[]>)` is 8 bytes, identical to the
raw pointer, and the generated code is the same. What it buys is that the
release is now tied to the object's lifetime rather than to a line of code that
has to be reached: exception-safe, and the `new[]`/`delete[]` pairing is
enforced by the type rather than remembered. `bench_pipe` is unchanged
(p50 0.25 µs) and runs clean under AddressSanitizer and LeakSanitizer.

### RAII for file descriptors — **in progress**

The remaining hand-managed resources are file descriptors. Two real gaps:

```cpp
listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);   // return value unchecked
...
if (::bind(...) != 0) return 0;                    // early return, fd left open
if (::listen(listen_fd_, 64) != 0) return 0;       // same
```

and `gateway_loop`'s `::close(fd)` sits at the bottom of the function — correct
today because there is exactly one exit path, but it becomes a per-connection
descriptor leak the moment anyone adds an early return.

The plan is a small `UniqueFd` type: exclusive ownership, copy deleted, move
allowed, `::close()` in the destructor. The same shape as `std::unique_ptr`,
with an `int` and `close()` in place of a pointer and `delete`.

Deliberately **not** wrapped: `client_fds_` and `Request::fd`. Both are
non-owning copies — `client_fds_` exists so `stop()` can call `shutdown()` on
each socket, and `Request::fd` tells the matching thread where to send the ack.
Wrapping either would give the same descriptor two owners and produce a double
`close()`, which is worse than a leak: descriptors are recycled, so the second
close would tear down an unrelated connection.

(`Request::fd` also *cannot* be wrapped — `MpscQueue` static-asserts
`is_trivially_copyable_v<T>`, and a type with a destructor isn't. The type
system already refuses to let ownership travel through the queue.)

### Known gaps not yet addressed

- **Duplicate order ids are not rejected.** `rest()` does
  `index_[order_id] = ...`, and `operator[]` overwrites. Submitting two orders
  with the same id silently orphans the first: it stays resting and tradeable,
  but no id reaches it, and a cancel hits the second. Not reachable through the
  gateway, which mints ids server-side, but `OrderBook` is a standalone unit
  with an unenforced precondition.
- **Time priority is under-tested.** Inverting `push_back` to `push_front` in
  `rest()` — which turns the book from FIFO to LIFO — fails exactly one test
  (`price_time_priority`, 2 of 109 checks). One of the two properties the book
  exists to provide is guarded by two assertions.
- **`next_producer_` is atomic but only ever touched by the single acceptor
  thread.** Harmless, but defensive rather than necessary; it should say which.
- **`stop()` relies on join ordering, not locking**, to make `gateways_` safe:
  the acceptor is joined before that vector is read. Correct, cheaper than a
  mutex, and currently undocumented — nothing stops someone reordering the two
  lines.

---

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

./build/matching_engine        # hand-driven in-process demo
./build/matching_server 9001   # standalone TCP gateway on 127.0.0.1:9001
```

**Requirements:** a C++20 compiler (tested on Apple clang 21), CMake ≥ 3.20, and
a POSIX platform for the gateway (Linux/macOS — it uses BSD sockets).

---

## Limitations

Named deliberately — these are the honest gaps, not oversights:

- **Benchmarked on macOS / Apple Silicon over loopback**, with no core pinning or
  isolation. Numbers would differ (likely lower median, far tighter tail) on
  tuned Linux with `isolcpus` and pinned threads over a real NIC.
- **Single symbol.** One book; no instrument routing.
- **No persistence or recovery** — the book is in-memory only.
- **No self-trade prevention.**
- **No market-data publication** — only per-order acks, no book/trade feed.
- **Blocking sockets, one gateway thread per connection** — correct and simple,
  not tuned for very high connection counts (no `epoll`/`kqueue` event loop).
- **The benchmark client predicts server-assigned order IDs** (it mirrors the
  single connection's counter) so it can issue cancels; a real client would learn
  IDs from acks instead.
- **No wire-level endianness normalisation** — both ends are assumed same-ABI
  (fine for loopback; a cross-host deployment must fix the wire to little-endian).
