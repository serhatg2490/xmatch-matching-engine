#pragma once

#include "xmatch/matching_engine_api.hpp"

namespace xmatch::detail {

// Tick ladder: the minimum price increment widens as the price rises, the
// way cash-equity venues generally arrange it. The four brackets below are a
// deliberately small, self-contained table chosen for this engine; a real
// venue's published schedule has considerably more of them, and swapping one
// in means editing this table and nothing else.
//
// All arithmetic is integer, in price units of 1/10000. Price math never goes
// through float or double.
//
//   0        <= price <  200000  (20.00) -> tick  100  (0.01)
//   200000   <= price <  500000  (50.00) -> tick  200  (0.02)
//   500000   <= price < 1000000 (100.00) -> tick  500  (0.05)
//   1000000  <= price                    -> tick 1000  (0.10)
Price tick_size(Price price);

// True iff price > 0 and price is aligned to the tick of its bracket.
bool is_tick_aligned(Price price);

Price ceil_to_tick(Price price);  // round UP to the nearest valid tick
Price floor_to_tick(Price price); // round DOWN to the nearest valid tick

} // namespace xmatch::detail
