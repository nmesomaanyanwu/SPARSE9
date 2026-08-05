// bench_tcp.cpp -- end-to-end order-to-ack latency over the full TCP path.
//
// This is the headline number. A client socket sends a length-prefixed order
// frame; the server's gateway thread parses it and enqueues it; the matching
// thread applies it to the book and writes an ack frame back; the client reads
// the ack. The sample is send()->recv(). Everything runs on one machine over
// the loopback interface, so it exercises the real socket syscall path but not
// a physical NIC (see README limitations).
//
// The server assigns producer-partitioned order ids. Because there is a single
// connection sending in order, the client predicts each assigned id (producer 1,
// incrementing counter) and remembers the ids of orders it rested so it can
// cancel them later -- driving the exact same steady-state workload as the other
// benchmarks.
//
// Usage: bench_tcp [num_ops] [target_depth] [seed]

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>

#include "matching_server.hpp"
#include "net.hpp"
#include "wire.hpp"
#include "latency.hpp"
#include "workload.hpp"

namespace
{
int connect_to(std::uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::perror("connect");
        std::exit(1);
    }
    net::set_nodelay(fd);
    return fd;
}

// Client-side mirror of the server's id assignment, plus a NewOrder send.
struct Client
{
    int fd;
    std::uint64_t wire_counter = 0; // mirrors gateway's per-connection counter

    OrderId send_new(const Order& o, std::uint64_t seq)
    {
        wire::NewOrder m;
        m.side = wire::from_side(o.side);
        m.order_type = wire::from_order_type(o.type);
        m.tif = wire::from_tif(o.time_in_force);
        m.price = o.price;
        m.quantity = o.quantity;
        m.client_seq = seq;
        net::send_frame(fd, &m, sizeof(m));
        return wire::make_order_id(1, ++wire_counter); // predicted assigned id
    }

    void send_cancel(OrderId wire_id, std::uint64_t seq)
    {
        wire::Cancel m;
        m.order_id = wire_id;
        m.client_seq = seq;
        net::send_frame(fd, &m, sizeof(m));
    }

    bool recv_ack(wire::Ack& out)
    {
        std::uint8_t buf[64];
        std::uint32_t len = 0;
        if (!net::recv_frame(fd, buf, sizeof(buf), len)) return false;
        std::memcpy(&out, buf, sizeof(out));
        return true;
    }
};
} // namespace

int main(int argc, char** argv)
{
    const std::size_t num_ops =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000ull;
    const std::size_t target_depth =
        (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000ull;
    const std::uint64_t seed =
        (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 42ull;

    std::printf("bench_tcp: ops=%zu target_depth=%zu seed=%llu (loopback)\n",
                num_ops, target_depth, (unsigned long long)seed);

    MatchingServer server;
    std::uint16_t port = server.listen(0);
    if (port == 0)
    {
        std::fprintf(stderr, "failed to bind\n");
        return 1;
    }
    server.start();

    Client cli{connect_to(port)};
    std::unordered_map<OrderId, OrderId> gen_to_wire; // rested gen id -> wire id
    std::uint64_t seq = 0;
    const std::uint64_t clock_overhead = bench::measure_clock_overhead_ns();

    // --- warm up to target depth ---
    {
        Workload warm(target_depth, seed);
        while (warm.pool_size() < target_depth)
        {
            Workload::Op op = warm.next();
            OrderId wid = cli.send_new(op.order, ++seq);
            wire::Ack ack;
            cli.recv_ack(ack);
            gen_to_wire[op.order.order_id] = wid;
            warm.record(op, true);
        }
    }

    // ----------------------------------------------------------------
    // 1. LATENCY: one order in flight, send -> ack over loopback.
    // ----------------------------------------------------------------
    {
        Workload wl(target_depth, seed);
        // Rebuild the same warm-up mapping the server already has, and advance
        // the client's wire counter past the warm-up NewOrders.
        std::unordered_map<OrderId, OrderId> lat_map = gen_to_wire;
        // wire_counter already past warm-up from the warm-up sends above.

        bench::Samples lat;
        lat.reserve(num_ops);
        while (wl.pool_size() < target_depth) wl.record(wl.next(), true);

        auto t0 = bench::Clock::now();
        for (std::size_t i = 0; i < num_ops; ++i)
        {
            Workload::Op op = wl.next();
            wire::Ack ack;
            if (op.kind == Workload::Op::Cancel)
            {
                auto it = lat_map.find(op.cancel_id);
                OrderId wid = (it != lat_map.end()) ? it->second : 0;
                auto a = bench::Clock::now();
                cli.send_cancel(wid, ++seq);
                cli.recv_ack(ack);
                auto b = bench::Clock::now();
                lat.add(bench::ns_between(a, b));
                if (it != lat_map.end()) lat_map.erase(it);
            }
            else
            {
                auto a = bench::Clock::now();
                OrderId wid = cli.send_new(op.order, ++seq);
                cli.recv_ack(ack);
                auto b = bench::Clock::now();
                lat.add(bench::ns_between(a, b));
                if (op.kind == Workload::Op::Add)
                    lat_map[op.order.order_id] = wid;
            }
            wl.record(op, op.kind == Workload::Op::Add);
        }
        auto t1 = bench::Clock::now();
        bench::report("order-to-ack end-to-end (TCP loopback, 1 in flight)", lat,
                      bench::ns_between(t0, t1) / 1e9, clock_overhead);
    }

    // ----------------------------------------------------------------
    // 2. THROUGHPUT: pipelined. Sender floods; receiver drains acks.
    // ----------------------------------------------------------------
    {
        std::atomic<bool> recv_done{false};
        std::size_t to_send = num_ops;

        std::thread receiver([&] {
            for (std::size_t i = 0; i < to_send; ++i)
            {
                wire::Ack ack;
                if (!cli.recv_ack(ack)) break;
            }
            recv_done.store(true, std::memory_order_release);
        });

        Workload wl(target_depth, seed);
        std::unordered_map<OrderId, OrderId> map2 = gen_to_wire;
        while (wl.pool_size() < target_depth) wl.record(wl.next(), true);

        auto t0 = bench::Clock::now();
        for (std::size_t i = 0; i < to_send; ++i)
        {
            Workload::Op op = wl.next();
            if (op.kind == Workload::Op::Cancel)
            {
                auto it = map2.find(op.cancel_id);
                cli.send_cancel(it != map2.end() ? it->second : 0, ++seq);
                if (it != map2.end()) map2.erase(it);
            }
            else
            {
                OrderId wid = cli.send_new(op.order, ++seq);
                if (op.kind == Workload::Op::Add) map2[op.order.order_id] = wid;
            }
            wl.record(op, op.kind == Workload::Op::Add);
        }
        receiver.join();
        auto t1 = bench::Clock::now();
        double secs = bench::ns_between(t0, t1) / 1e9;
        std::printf("\n=== saturated throughput (TCP loopback, pipelined) ===\n");
        std::printf("  operations : %zu\n", to_send);
        std::printf("  wall time  : %.3f s\n", secs);
        std::printf("  throughput : %.0f ops/sec\n", to_send / secs);
    }

    ::close(cli.fd);
    server.stop();
    return 0;
}
