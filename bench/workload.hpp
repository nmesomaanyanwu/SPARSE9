// workload.hpp -- the one order stream every benchmark replays.
//
// Keeping a single generator means bench_core (in-process, no queue),
// bench_pipe (queue + matching thread) and bench_tcp (full loopback path) all
// push byte-for-byte the same sequence of operations. Any latency difference
// between them is therefore transport, not a different workload.
//
// Steady state: the book is held near `target_depth` resting orders.
//   * below target  -> ADD a resting (non-crossing) limit
//   * at/above       -> coin flip: TAKE (marketable IOC, crosses) or CANCEL
// Prices random-walk within +/-band of a fixed mid. Fully seeded -> reproducible.
#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "order.hpp"

class Workload
{
public:
    struct Op
    {
        enum Kind { Add, Take, Cancel } kind;
        Order order;          // valid for Add / Take
        OrderId cancel_id;    // valid for Cancel
        std::size_t slot;     // pool slot chosen for Cancel (internal use)
    };

    Workload(std::size_t target_depth, std::uint64_t seed)
        : target_(target_depth), rng_(seed)
    {
        live_.reserve(target_depth * 2);
    }

    // Decide the next operation from the current pool size. Does not mutate the
    // pool for Add/Take; for Cancel it selects (but does not yet remove) a slot.
    Op next()
    {
        if (live_.size() < target_)
        {
            const bool buy = coin_(rng_);
            const OrderId id = next_id_++;
            const Price px = buy ? mid_ - 1 - off_(rng_) : mid_ + 1 + off_(rng_);
            Op op{Op::Add,
                  Order{id, OrderType::LIMIT, buy ? OrderSide::BUY : OrderSide::SELL,
                        px, qty_(rng_), TimeInForce::GTC},
                  0, 0};
            return op;
        }
        if (coin_(rng_))
        {
            const bool buy = coin_(rng_);
            const OrderId id = next_id_++;
            const Price px = buy ? mid_ + band_ : mid_ - band_;
            Op op{Op::Take,
                  Order{id, OrderType::LIMIT, buy ? OrderSide::BUY : OrderSide::SELL,
                        px, qty_(rng_), TimeInForce::IOC},
                  0, 0};
            return op;
        }
        std::uniform_int_distribution<std::size_t> pick(0, live_.size() - 1);
        std::size_t slot = pick(rng_);
        return Op{Op::Cancel, {}, live_[slot], slot};
    }

    // Feed back the result so the pool stays accurate.
    //   Add : rested == (no trades generated)  -> track id if it rested
    //   Cancel: always removes the chosen slot (matches the fill/cancel race)
    void record(const Op& op, bool rested_or_ignored)
    {
        switch (op.kind)
        {
        case Op::Add:
            if (rested_or_ignored) live_.push_back(op.order.order_id);
            break;
        case Op::Take:
            break;
        case Op::Cancel:
            live_[op.slot] = live_.back();
            live_.pop_back();
            break;
        }
    }

    std::size_t pool_size() const { return live_.size(); }

private:
    std::size_t target_;
    std::mt19937_64 rng_;
    std::vector<OrderId> live_;
    OrderId next_id_ = 1;

    static constexpr Price mid_ = 100'000;
    static constexpr Price band_ = 500;
    std::uniform_int_distribution<Price> off_{0, band_ - 1};
    std::uniform_int_distribution<Quantity> qty_{1, 50};
    std::uniform_int_distribution<int> coin_{0, 1};
};
