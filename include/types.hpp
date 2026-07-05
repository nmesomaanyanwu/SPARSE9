#pragma once
#include <cstdint>

using Price = std::int64_t; // integer ticks, signed (see Phase 0 notes on why)
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
