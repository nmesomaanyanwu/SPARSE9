# The 14 Microseconds That Weren't My Matching Engine

I spent weeks making an order book fast. It matches an order in 84 nanoseconds.

Then I put it behind a TCP socket, measured the round trip, and got **14.6 microseconds**.

The matching was 0.6% of it. Everything else — 99.4% — was the part I'd never learned:
the network layer. This post is what I found when I went and learned it.

---

## Where I was starting from

I want to be straight about the starting point, because I think posts like this
usually skip it.

I had built a limit order book. I understood price-time priority, maker-price
execution, IOC and FOK semantics. I could trace an order through the matching
loop by hand. That part was mine.

I did not know what a socket was. I could not have told you what a file
descriptor was — I'd have guessed "metadata about a file," which is wrong. I
had never written a line of threaded code. When I read the word *syscall* I
knew it expanded to "system call" and that was the end of my knowledge.

So the gateway sitting in front of my matching engine was a black box to me.
Making it not a black box is what the last stretch has been.

---

## Your program is not allowed to touch the network

This is the fact everything else hangs off, and nobody had ever said it to me
plainly.

Your process cannot talk to a network card. It cannot read a disk or draw on the
screen. It can do arithmetic and move data around its own memory, and that is
the complete list. This isn't a C++ limitation — the CPU enforces it. Your code
runs in **user mode**, where instructions that touch hardware are illegal. The
**kernel** runs in kernel mode, where they aren't.

Think of your program as a guest in a hotel room. You can rearrange the
furniture all you like. You cannot walk out and start rewiring the building's
phone system — there's one phone system and hundreds of guests, and if everyone
could grab the cables directly it would be chaos.

So there's a front desk. Want an outside line? Ask the front desk.

**A syscall is calling the front desk.** And here's the thing that reframed the
whole project for me: `send(fd, buf, len, 0)` *looks* like an ordinary function
call. It isn't. It executes a special CPU instruction that traps into the
kernel, switches privilege level, saves your registers, switches to a kernel
stack, does the work, and switches everything back.

A normal function call is a jump and a return — a couple of nanoseconds. A
syscall is **hundreds of nanoseconds before it does anything useful**, more on
machines with Spectre/Meltdown mitigations, which flush caches on the boundary.

Now count the syscalls in one order round trip on my server:

1. client `send()` — syscall
2. gateway thread, asleep in `recv()`, has to be **woken by the scheduler** — microseconds
3. gateway `recv()` returns — syscall
4. hand off to the matching thread — another wake-up
5. matcher `send()`s the ack — syscall
6. client wakes, its `recv()` returns — syscall

Four syscalls and two thread wake-ups. **The wake-ups are the expensive part** —
getting a sleeping thread back onto a CPU means going through the OS scheduler,
and that's microsecond-scale work on a general-purpose operating system.

That's the 14.6 µs. Not my matching engine. The cost of leaving the room.

---

## A file descriptor is a ticket number

When you ask the front desk to open something, they don't hand you the thing.
They hand you a small integer and write in their ledger what it refers to.

```
your process                 kernel's table for your process
  int fd = 4;   ───────────▶  [0] stdin
                              [1] stdout
                              [2] stderr
                              [3] ──▶ the listening socket
                              [4] ──▶ TCP connection to a client
```

That's why every networking function takes an `int` as its first argument.
You're quoting your ticket number. It's also why the first socket you open is
usually 3 — 0, 1 and 2 are already issued to every Unix process at birth.

Two things about fds that bit me conceptually:

**They're recycled.** Close fd 3 and the next thing you open gets fd 3 again.
I briefly thought the fd could serve as a per-client identity — it can't. A
client's orders keep resting in the book long after it disconnects, and the next
client to connect will inherit its number. Order ids must be unique for the
lifetime of the *book*; fds are only unique among things *currently open*.

**They're not sockets.** They're an index into a table of *everything* open —
files, pipes, terminals, sockets. That uniformity is Unix's central idea
("everything is a file") and it's why `select()` can wait on a socket at all:
`select` doesn't know or care what kind of thing an fd refers to.

---

## TCP does not preserve message boundaries

This is the one that made me rewrite my mental model.

TCP gives you three guarantees: your bytes arrive, they arrive in order, and
they aren't corrupted. It gives you nothing else. In particular:

> TCP does not tell you where one message ends and the next begins.

I wrote a test to convince myself. The client makes **five separate `send()`
calls**, 20 bytes each. The server does one `recv()`:

```
[server] recv() call #1 returned 100 bytes:
  "MSG-AAAAAAAAAAAAAAA_MSG-BBBBBBBBBBBBBBB_MSG-CCC..."
[server] 5 send() calls arrived in 1 recv() call(s).
```

One `recv`. One hundred bytes. All five messages fused into a single blob with
nothing marking the seams. And it could just as easily have gone the other way —
7 bytes, then 93. Or one byte, a hundred times.

The way I think about it now: you're dictating a letter over the phone one
letter at a time, with no pauses and no punctuation. `THEDOGSATONTHEMAT`. Every
character arrives, in order, perfectly. The listener has no idea where the words
are. `send()` does not put your data in an envelope — it pours it into a pipe.

This is the classic beginner TCP bug, and it's nasty because the naive server
**works in testing**. Small messages, sent one at a time, on a fast local link,
each `send` happening to land in its own `recv`. Then it corrupts data in
production the first time traffic is busy enough for two messages to merge.

### So you put the envelopes back yourself

That's called **framing**, and there are three standard ways: fixed-size
messages, a delimiter byte, or a length prefix. I went with the length prefix,
which is the usual choice for binary protocols:

```
[uint32 payload_length][payload bytes]
```

The receiver's job becomes unambiguous: read exactly 4 bytes to learn N, read
exactly N bytes to get one message, repeat. No scanning, no escaping, any byte
value allowed in the payload, and you know the size before you read it.

Here's a real ack frame off my wire:

```
[len prefix] 20 00 00 00                 = 32
[payload]    03 00 00 00 00 00 00 00     type=3 (Ack), status=0 (ok), padding
             07 00 00 00 00 00 01 00     order_id = 0x0001000000000007
             02 00 00 00 00 00 00 00     fills = 2
             29 00 00 00 00 00 00 00     client_seq = 41
total: 36 bytes
```

Two details in there I like:

**`order_id` is news to the client.** Clients don't pick their own ids — the
gateway mints them. The ack is how a client learns what its order is called,
which it needs in order to cancel it later.

**`client_seq` is echoed back untouched.** It's how a client with several orders
in flight pairs each reply to the request that caused it. Without it, pipelining
is impossible.

The bytes read backwards — least significant first — because that's little-endian,
how x86 and ARM store integers natively. Both ends of my connection are the same
machine, so it works. A cross-host deployment would have to normalise this, and
I've listed that as a known gap rather than pretending it's handled.

---

## `recv()` doesn't give you what you asked for

The last piece of the byte-stream problem. `recv(fd, buf, 100, 0)` does not mean
"give me 100 bytes." It means "give me **up to** 100 bytes, whatever's available
right now." Ask for 100, get 40. That's a **short read**, and it's normal
behaviour, not an error.

So every read has to loop. I traced mine reading a 32-byte message that the
sender dribbled out in three pieces:

```
read_n: need n=32 bytes, buffer starts at 0xe91ffc5de808
  loop 1: recv(fd, p+0,  32) returned r=12    got is now 12 of 32
  loop 2: recv(fd, p+12, 20) returned r=8     got is now 20 of 32
  loop 3: recv(fd, p+20, 12) returned r=12    got is now 32 of 32
  done. 3 recv() calls to get 32 bytes.
```

`p + got` is where to write next. `n - got` is how much is still missing. You
keep going until the buffer is full.

The return value carries three different meanings and you have to handle all
three: **positive** means that many bytes arrived, **zero** means the peer closed
the connection cleanly, **negative** means an error. That's why the type is
`ssize_t` and not `size_t` — it has to be signed to return -1. If it were
unsigned, an error would read as a gigantic positive number and your buffer
offset would explode.

`send()` has exactly the same property, which is why writing loops too.

---

## The thread architecture, and why the book has no locks

Here's the part where networking stopped being a separate topic and started
being an architecture problem.

My server has, with three clients connected, **six threads**:

| Thread | How many | What it does, forever |
|---|---|---|
| main | 1 | starts everything, then waits |
| acceptor | 1 | `select` → `accept` → register fd → spawn a gateway |
| gateway | **1 per connection** | `recv_frame` → parse → build a Request → enqueue |
| matcher | 1 | poll the queue → mutate the book → send the ack |

Two things I had wrong for an embarrassingly long time.

**`accept()` does not read any bytes.** It returns an integer. The receptionist
notices the phone is flashing (`select`), picks up and transfers the caller to
extension 4 (`accept`), and goes straight back to watching the switchboard. She
does not listen to the conversation. The conversation happens on extension 4 —
that's the gateway thread, which doesn't exist until the acceptor creates it.

**`accept()` runs once per connection, not once per order.** A client that
connects at 9am and sends four million orders before lunch caused exactly one
`accept()` and four million `recv_frame()`s.

### The book is not thread-safe, and I'm not going to make it

My `OrderBook` has no synchronisation in it at all. Not a mutex, not an atomic,
not one. I grepped to be sure — the only match for "mutex|atomic|lock" in the
entire matching core is a code comment.

Two threads calling `submit()` at once would interleave writes into a red-black
tree mid-rotation and shred it. To see how quietly this fails, I ran my trade-id
counter — a plain `static` integer — from two threads:

```
trades created      : 100000
DISTINCT trade ids  : 70911
DUPLICATE trade ids : 29089
```

Nothing crashed. Nothing warned. Twenty-nine thousand trades sharing ids with
other trades. I ran it again and got 7,078 duplicates — same binary, seconds
apart. That's a data race: silent, nondeterministic, and not reproducible.

So the book needs protecting. The design I landed on is: **don't share it**.

Exactly one thread — the matching thread — ever touches the book. Gateway
threads parse bytes and drop a small plain-data `Request` struct into a bounded
lock-free queue. The matcher pulls them out one at a time.

The waiters don't walk into the kitchen and start cooking. They clip tickets to
a rail. One chef works the stove.

And a detail I like: the queue is FIFO, so **arrival order into the matcher is
preserved**, which means time priority survives the multithreading for free.

### The queue doesn't notify anybody

Worth stating because I got this wrong when explaining it back to myself: the
queue is a passive array in memory. It has no agency. The matching thread sits
in a tight loop asking *"anything for me?"* millions of times a second, and the
answer is almost always no.

That's polling, and it means the matching thread burns **100% of a CPU core**
doing nothing most of the time. The alternative is a condition variable — the
matcher sleeps, a producer wakes it. Polite, frees the core, and costs you
microseconds on every single order.

Burn a core to save microseconds. For a matching engine that's obviously worth
it. For a web server it obviously isn't. My gateway threads make the opposite
choice and block in `recv`, because they'd otherwise burn a core each while idle.

Two ways of waiting, chosen for different reasons, in the same program.

---

## What I rejected, and why

**A global lock over the whole book.** One mutex around every operation. This is
*correct* — I want to be clear about that, because "rejected" can sound like
"wrong." It would produce right answers every time, and for most systems it's
the right first move.

I ruled it out on cost. Every producer serialises on one mutex, and the critical
section is the entire match — potentially a sweep across several price levels.
An *uncontended* mutex acquire is already the same order of magnitude as my 84 ns
of matching work. A *contended* one means sleeping and waking: microseconds.

And there's a failure mode specific to latency: a thread can be **descheduled by
the OS while holding the lock**. The kernel doesn't know it's in a critical
section. Now everyone waiting is stuck, not for the length of the critical
section, but until the scheduler gets round to that thread again. That's how a
system with a nanosecond median grows a millisecond tail.

**Per-price-level locking.** One lock per level, so orders at different prices
proceed in parallel. This one breaks on a case that's completely routine:
**a marketable order sweeps multiple levels.** It has to hold or hand off locks
across all of them in a consistent order, or you deadlock. Levels are created and
destroyed constantly, so you need a lock over the map to protect the level locks
— and now you have a global lock again, one layer up.

The fatal part isn't performance though. Once independent levels progress
concurrently, **time priority is no longer guaranteed by arrival order** — which
is the entire property an order book exists to provide.

**A lock-free book.** CAS all the way down. I ruled this out on tractability
rather than on merit: ABA hazards, acquire/release ordering between price-level
and node updates, and safe memory reclamation (you can't free a node while
another thread might still be reading it, and knowing when it's safe requires
hazard pointers or epoch-based reclamation).

That's a research-grade subsystem, and I couldn't have convinced myself it was
correct or tested it into confidence. I'd rather ship something I can reason
about.

**The cost of what I picked:** matching is single-threaded, permanently. One
core's worth of throughput per book. You scale by running one matching thread
per *symbol*, not by parallelising one book.

---

## What's actually wrong with my gateway

Now that I understand it, I can name the limitations properly instead of
hand-waving.

**One thread per connection doesn't scale.** This is the real ceiling, and it
isn't about `select` — I only use `select` on the listening socket, watching a
single fd. The problem is that ten thousand clients means ten thousand threads,
each with its own stack, and the scheduler thrashing between them. Thread
creation is tens of microseconds and context switches aren't free.

The fix is an **event loop**: one thread watching thousands of connections,
handling whichever are ready. `epoll` on Linux, `kqueue` on macOS/BSD. Both are
O(1) in the number of watched descriptors because the kernel keeps your interest
set registered, instead of you re-passing the whole list on every call — which is
what `select` does, and why `select` is O(n) and capped at 1024 descriptors.

Beyond that there's **`io_uring`** on Linux: shared ring buffers between kernel
and userspace so you can submit and reap I/O with *no syscall at all* in the
common case. Which is, satisfyingly, the same idea as my MPSC queue — a lock-free
ring to avoid an expensive hand-off — applied to the kernel boundary instead of a
thread boundary.

And past that, **kernel bypass** (DPDK, Solarflare `ef_vi`): skip the kernel
entirely, talk to the NIC from userspace. That's what actual HFT firms run and
it's how you get sub-microsecond wire-to-wire.

**My benchmarks are over loopback on macOS with no core pinning.** No `isolcpus`
equivalent exists on macOS, so my threads float across cores under the normal
scheduler. My p99 is roughly 3× my median, and I believe that's scheduler jitter
rather than anything in my code — but *believe* is the honest word. To confirm
it I'd need to sample context-switch events and correlate them with the tail,
then re-run pinned on Linux and see whether the tail collapses.

---

## What I actually took away

The thing I keep coming back to is that **nothing in the network layer is
mysterious — it's just a layer I'd never been shown.** Every piece of it is
mechanical. Bytes go in a pipe. The pipe doesn't mark where messages end, so you
mark them yourself. Reads come back short, so you loop. The kernel is expensive
to call, so you count your calls.

The second thing: **the interesting decisions in a system are almost never in the
algorithm.** My matching core was the part I was proud of, and it turned out to
be 0.6% of the latency. All the load-bearing choices — single writer, bounded
lock-free queue, spin versus block, thread per connection — live in the plumbing.

And the third: I got several of these wrong out loud before I got them right. I
thought threads flowed through the queue. I thought `accept()` parsed bytes. I
thought a file descriptor was per-order rather than per-connection. Writing those
down and then finding out why they're wrong is, as far as I can tell, the actual
mechanism of learning this stuff.

---

*Code: [SPARSE9](https://github.com/nmesomaanyanwu/SPARSE9). Numbers in this post
are from an Apple M5 running macOS, over the loopback interface, with no core
pinning or isolation — conditions that matter a lot, and which the repo's README
documents in full.*
