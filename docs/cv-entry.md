# SPARSE9 — CV entry (honest, measured)

Drop-in replacement for the résumé bullet block. Every figure here is produced
by a benchmark in this repo and reproducible with the commands in the README.
Keep the conditions attached to the numbers — an interviewer respects a
qualified figure and punishes a bare one.

---

**SPARSE9 — Limit Order Book & Matching Engine** · C++20, TCP/IP, CMake · 2026 – present

- Built a matching engine with full **price-time priority**, maker-price
  execution and LIMIT, MARKET, IOC and FOK orders; achieved **O(1) cancellation**
  through an `OrderId → Locator` index holding stable `std::list` iterators, so a
  cancel never searches the book.
- Measured the matching core at a **median of 84 ns / p99 584 ns per order over
  10 M orders** (single thread, in-process, seeded steady-state workload); wrapped
  in the full network path, **end-to-end order-to-ack is a median of 14.6 µs / p99
  43.7 µs over TCP loopback** (Apple M5, macOS, single producer, no core pinning —
  see repo for exact conditions).
- Implemented a TCP order-entry gateway over a **binary length-prefixed wire
  protocol** with producer-partitioned 64-bit order IDs, isolating networking from
  the matching core through a bounded **multi-producer, single-consumer queue**
  (Vyukov ring).
- Designed a **single-writer architecture** in which one thread exclusively owns
  book memory while gateway threads enqueue requests, structurally eliminating four
  failure modes: torn quantity updates, dangling best-price pointers, scrambled
  time priority and deadlock.
- Rejected global locking, per-price-level locking and a lock-free shared book
  after analysing contention, fairness, CAS/ABA hazards, acquire-release ordering
  and memory reclamation; layered the chosen concurrency and networking design on
  top of the correctness-first core **without modifying it or its 23-test suite**.

---

## Notes for whoever reads this before pasting it in

- The old draft quoted **4.8 µs median / 11.6 µs p99 end-to-end**. Those were
  never measured — there was no benchmark harness, no gateway and no concurrency
  in the repo when they were written. The real measured end-to-end median on this
  machine is **~14.6 µs**. Do not put the 4.8 µs number back; you cannot reproduce
  it and it will not survive an interview.
- If you later run this on **tuned Linux with pinned/isolated cores over a real
  NIC**, re-measure — the median will likely drop and the tail (currently ~3× the
  median from scheduler jitter) should tighten substantially. Update the numbers
  and the conditions together.
- The **84 ns core** number is the strong, defensible headline. Lead with it, and
  use the end-to-end figure to show you understand that the socket path — not the
  matching — dominates real latency. That framing is worth more than a smaller,
  unexplained number.
- Two honesty caveats already noted in the README's Limitations, in case they come
  up: the benchmark uses a single connection and the client predicts server-assigned
  order IDs to issue cancels; and the wire format is same-host (no endianness
  normalisation).
