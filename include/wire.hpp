// wire.hpp -- the binary, length-prefixed order-entry protocol.
//
// Every frame on the wire is:  [uint32 payload_len][payload bytes]
// The length prefix lets the reader pull exactly one message off a stream
// socket without guessing message boundaries. All multi-byte fields are written
// host-endian (both ends here are the same machine over loopback); a real
// cross-host deployment would fix this to little-endian on the wire.
//
// Producer-partitioned order ids: the top 16 bits are a producer id, the low 48
// bits a per-producer counter. Each gateway connection owns a producer id, so
// producers mint globally-unique 64-bit ids with no coordination.
#pragma once

#include <cstdint>
#include <cstring>

#include "order.hpp"

namespace wire
{
enum class MsgType : std::uint8_t { NewOrder = 1, Cancel = 2, Ack = 3 };

// Fixed-size POD payloads. We memcpy these straight into the frame body; they
// are trivially copyable and both ends share ABI (same machine).
#pragma pack(push, 1)
struct NewOrder
{
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::NewOrder);
    std::uint8_t side;      // 0 = BUY, 1 = SELL
    std::uint8_t order_type; // 0 = LIMIT, 1 = MARKET
    std::uint8_t tif;       // 0 = GTC, 1 = Day, 2 = IOC, 3 = FOK
    std::int64_t price;
    std::int64_t quantity;
    std::uint64_t client_seq; // echoed back in the ack
};

struct Cancel
{
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::Cancel);
    std::uint8_t pad[7] = {};
    std::uint64_t order_id;
    std::uint64_t client_seq;
};

struct Ack
{
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::Ack);
    std::uint8_t status; // 0 = accepted/ok, 1 = rejected/unknown
    std::uint8_t pad[6] = {};
    std::uint64_t order_id;   // id assigned (NewOrder) or targeted (Cancel)
    std::uint64_t fills;      // number of trades generated
    std::uint64_t client_seq; // echoes the request
};
#pragma pack(pop)

constexpr std::uint32_t kLenPrefix = 4;

// --- producer-partitioned ids ---
inline OrderId make_order_id(std::uint16_t producer, std::uint64_t counter)
{
    return (static_cast<OrderId>(producer) << 48) |
           (counter & 0x0000'FFFF'FFFF'FFFFull);
}

// --- enum <-> wire byte helpers ---
inline OrderSide to_side(std::uint8_t b)
{
    return b == 0 ? OrderSide::BUY : OrderSide::SELL;
}
inline OrderType to_order_type(std::uint8_t b)
{
    return b == 0 ? OrderType::LIMIT : OrderType::MARKET;
}
inline TimeInForce to_tif(std::uint8_t b)
{
    switch (b)
    {
    case 0: return TimeInForce::GTC;
    case 1: return TimeInForce::Day;
    case 2: return TimeInForce::IOC;
    default: return TimeInForce::FOK;
    }
}
inline std::uint8_t from_side(OrderSide s)
{
    return s == OrderSide::BUY ? 0 : 1;
}
inline std::uint8_t from_order_type(OrderType t)
{
    return t == OrderType::LIMIT ? 0 : 1;
}
inline std::uint8_t from_tif(TimeInForce t)
{
    switch (t)
    {
    case TimeInForce::GTC: return 0;
    case TimeInForce::Day: return 1;
    case TimeInForce::IOC: return 2;
    default: return 3;
    }
}
} // namespace wire
