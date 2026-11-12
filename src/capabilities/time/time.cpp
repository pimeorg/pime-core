#include "time.hpp"

#include "platform/platform.hpp"

namespace pime::cap {

uint64_t time_query(uint32_t kind) {
    switch (kind) {
        case 0: return platform::monotonic_ms();
        case 1: return platform::wall_ms();
        case 2:
            return static_cast<uint64_t>(
                static_cast<int64_t>(platform::timezone_offset_minutes()));
        default: return 0;
    }
}

}  // namespace pime::cap
