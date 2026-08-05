// net.hpp -- minimal blocking TCP helpers and length-prefixed framing.
//
// POSIX sockets only (Linux/macOS). Everything here is blocking I/O: fine for a
// single-connection latency benchmark and a small server. read_n / write_n loop
// until the full byte count is transferred or the peer goes away, because a
// stream socket may hand back a short read/write at any time.
#pragma once

#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{
// Read exactly n bytes. Returns false on EOF/error.
inline bool read_n(int fd, void* buf, std::size_t n)
{
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < n)
    {
        ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// Write exactly n bytes. Returns false on error.
inline bool write_n(int fd, const void* buf, std::size_t n)
{
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t sent = 0;
    while (sent < n)
    {
        ssize_t w = ::send(fd, p + sent, n - sent, 0);
        if (w <= 0) return false;
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

// Disable Nagle -- essential for a latency benchmark, otherwise small frames
// are batched and the numbers are meaningless.
inline void set_nodelay(int fd)
{
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Send one length-prefixed frame: [uint32 len][payload].
inline bool send_frame(int fd, const void* payload, std::uint32_t len)
{
    if (!write_n(fd, &len, sizeof(len))) return false;
    return write_n(fd, payload, len);
}

// Receive one frame into buf (capacity cap). Sets out_len. Returns false on EOF.
inline bool recv_frame(int fd, void* buf, std::uint32_t cap, std::uint32_t& out_len)
{
    std::uint32_t len = 0;
    if (!read_n(fd, &len, sizeof(len))) return false;
    if (len > cap) return false;
    if (!read_n(fd, buf, len)) return false;
    out_len = len;
    return true;
}
} // namespace net
