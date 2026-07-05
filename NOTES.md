# Matching Engine


## Phase 0 - skeleton 

### include/

- `side.hpp` - enum class side { Buy , Sell }
- `order_type` - enum class side {limit , Market}
- `time_in_force.hpp` — enum class TimeInForce { GTC, Day, IOC, FOK }
- `order.hpp` — struct Order: order_id, side, order_type, price (ticks), quantity, time_in_force
- `trade.hpp` — struct Trade: trade_id, buy_order_id, sell_order_id, price, quantity

### src/

- `main.cpp` — entry point, wires things together for now
- (order_book.cpp, matching_engine.cpp come in later phases)

