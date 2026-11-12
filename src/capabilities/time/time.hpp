#ifndef PIME_CAPABILITIES_TIME_HPP
#define PIME_CAPABILITIES_TIME_HPP

#include <cstdint>

namespace pime::cap {

/* kind: 0 monotonic ms, 1 wall ms, 2 timezone offset minutes */
uint64_t time_query(uint32_t kind);

}  // namespace pime::cap

#endif
