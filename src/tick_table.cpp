#include "engine/tick_table.hpp"

namespace xmatch::detail {

Price tick_size(Price price) {
    if (price < 200000) return 100;
    if (price < 500000) return 200;
    if (price < 1000000) return 500;
    return 1000;
}

bool is_tick_aligned(Price price) {
    if (price <= 0) return false;
    return price % tick_size(price) == 0;
}

Price ceil_to_tick(Price price) {
    if (price <= 0) return tick_size(0);
    Price step = tick_size(price);
    Price rem = price % step;
    return rem == 0 ? price : price + (step - rem);
}

Price floor_to_tick(Price price) {
    if (price <= 0) return 0;
    Price step = tick_size(price);
    Price rem = price % step;
    return price - rem;
}

} // namespace xmatch::detail
