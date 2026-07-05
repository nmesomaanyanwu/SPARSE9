#pragma once

#include <cstdint>


// this is the time you can leave an order to execute if it doesnt fill immediately

enum class TimeInForce : std::int8_t
{
    GTC, // Good Till Cancelled - rests in the book indefinently
    IOC, // Immediate or Cancel - fill whatever you can and cancel the rest
    Day, // Day -Rests in the book until the end of the trading day
    FOK  // Fill or  Kill - fill the entire order or cancel the whole thing . Never partially fills

};
