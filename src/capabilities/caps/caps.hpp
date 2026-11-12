#ifndef PIME_CAPABILITIES_CAPS_HPP
#define PIME_CAPABILITIES_CAPS_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <vector>

#include "codec/cbor.hpp"
#include "capabilities/permission/permission.hpp"

namespace pime::cap {

/* caps.query: granted plugin capabilities plus core ABI/host bits. */
codec::Value caps_query(const Permission& permission, const std::string& alias,
                        uint32_t host_caps);

}  // namespace pime::cap

#endif
