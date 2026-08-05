// matching_server.hpp -- the TCP order-entry gateway wrapped around the
// single-writer matching core.
//
// Topology:
//
//   client sockets        gateway threads (1 per conn)     matching thread
//   ----------------  bytes  ----------------------  MPSC  -----------------
//   producer 1     -------->  read frame, parse,   ------>  owns the book,
//   producer 2     -------->  assign order id,     ------>  submit/cancel,
//      ...                    push request                 write ack to fd
//
// Exactly one thread (the matching thread) ever touches the OrderBook. Gateway
// threads only parse bytes and enqueue. The matching thread writes the ack back
// to the originating socket; the matching *core* (OrderBook) still knows nothing
// about sockets, framing or clients -- that isolation is the whole point.
//
// Blocking sockets, one gateway thread per connection: simple and correct, not
// tuned for tens of thousands of connections (see README limitations).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/select.h>

#include "mpsc_queue.hpp"
#include "net.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "wire.hpp"

class MatchingServer
{
    // One unit of work crossing the single-writer boundary.
    struct Request
    {
        bool is_cancel;
        Order order;         // valid when !is_cancel
        OrderId cancel_id;   // valid when is_cancel
        int fd;              // socket to ack on
        std::uint64_t client_seq;
    };

public:
    explicit MatchingServer(std::size_t queue_capacity = (1 << 16))
        : queue_(queue_capacity)
    {
    }

    // Bind + listen. port 0 picks an ephemeral port; the chosen port is
    // returned so a caller (e.g. the benchmark) can connect to it.
    std::uint16_t listen(std::uint16_t port)
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return 0;
        if (::listen(listen_fd_, 64) != 0) return 0;

        socklen_t alen = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
        return ntohs(addr.sin_port);
    }

    // Start the matching thread and the accept loop (accept loop runs on its
    // own thread so start() returns immediately).
    void start()
    {
        running_.store(true, std::memory_order_relaxed);
        matcher_ = std::thread([this] { match_loop(); });
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    void stop()
    {
        running_.store(false, std::memory_order_relaxed);
        // Wake any gateway thread parked in a blocking recv() by shutting down
        // its socket (this DOES unblock recv on macOS, unlike shutdown on a
        // listening socket vs accept -- so the accept loop instead polls via
        // select with a short timeout).
        {
            std::lock_guard<std::mutex> lk(fds_mu_);
            for (int fd : client_fds_) ::shutdown(fd, SHUT_RDWR);
        }
        if (acceptor_.joinable()) acceptor_.join();
        for (auto& t : gateways_)
            if (t.joinable()) t.join();
        if (matcher_.joinable()) matcher_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

private:
    // Wait up to timeout_ms for fd to become readable while running_. Returns
    // true if readable, false on timeout/stop (caller should re-check running_).
    bool wait_readable(int fd, int timeout_ms)
    {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        return ::select(fd + 1, &rs, nullptr, nullptr, &tv) > 0;
    }

    void accept_loop()
    {
        while (running_.load(std::memory_order_relaxed))
        {
            if (!wait_readable(listen_fd_, 100)) continue;
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            net::set_nodelay(fd);
            {
                std::lock_guard<std::mutex> lk(fds_mu_);
                client_fds_.push_back(fd);
            }
            std::uint16_t producer = next_producer_++;
            gateways_.emplace_back([this, fd, producer] { gateway_loop(fd, producer); });
        }
    }

    // Per-connection reader: parse frames, mint ids, enqueue requests.
    void gateway_loop(int fd, std::uint16_t producer)
    {
        std::uint64_t counter = 0;
        alignas(8) std::uint8_t buf[256];
        std::uint32_t len = 0;
        // Blocking recv on the hot path (no per-frame select syscall). stop()
        // unblocks us by shutting the socket down.
        while (net::recv_frame(fd, buf, sizeof(buf), len))
        {
            if (len < 1) continue;
            const auto type = static_cast<wire::MsgType>(buf[0]);
            Request req{};
            req.fd = fd;

            if (type == wire::MsgType::NewOrder && len >= sizeof(wire::NewOrder))
            {
                wire::NewOrder m;
                std::memcpy(&m, buf, sizeof(m));
                const OrderId id = wire::make_order_id(producer, ++counter);
                req.is_cancel = false;
                req.order = Order{id, wire::to_order_type(m.order_type),
                                  wire::to_side(m.side), m.price, m.quantity,
                                  wire::to_tif(m.tif)};
                req.client_seq = m.client_seq;
            }
            else if (type == wire::MsgType::Cancel && len >= sizeof(wire::Cancel))
            {
                wire::Cancel m;
                std::memcpy(&m, buf, sizeof(m));
                req.is_cancel = true;
                req.cancel_id = m.order_id;
                req.client_seq = m.client_seq;
            }
            else
            {
                continue; // malformed -> drop
            }

            while (running_.load(std::memory_order_relaxed) && !queue_.enqueue(req))
                cpu_relax();
        }
        ::close(fd);
    }

    // The single writer. Drains the queue, mutates the book, acks the client.
    void match_loop()
    {
        Request r;
        while (running_.load(std::memory_order_relaxed))
        {
            if (!queue_.try_dequeue(r))
            {
                cpu_relax();
                continue;
            }
            wire::Ack ack{};
            if (r.is_cancel)
            {
                const bool ok = book_.cancel(r.cancel_id);
                ack.status = ok ? 0 : 1;
                ack.order_id = r.cancel_id;
                ack.fills = 0;
            }
            else
            {
                const auto trades = book_.submit(r.order);
                ack.status = 0;
                ack.order_id = r.order.order_id;
                ack.fills = trades.size();
            }
            ack.client_seq = r.client_seq;
            net::send_frame(r.fd, &ack, sizeof(ack));
        }
    }

    static void cpu_relax()
    {
#if defined(__aarch64__) || defined(__arm64__)
        asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
        asm volatile("pause" ::: "memory");
#endif
    }

    OrderBook book_;
    MpscQueue<Request> queue_;
    int listen_fd_ = -1;
    std::mutex fds_mu_;
    std::vector<int> client_fds_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> next_producer_{1};
    std::thread matcher_;
    std::thread acceptor_;
    std::vector<std::thread> gateways_;
};
