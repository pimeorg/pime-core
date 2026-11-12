#include "caps.hpp"

namespace pime::cap {

codec::Value caps_query(const Permission& permission, const std::string& alias,
                        uint32_t host_caps) {
    codec::Value out = codec::Value::map_v();
    codec::Value list = codec::Value::array_v();
    for (const auto& cap : permission.granted(alias)) list.push(codec::Value::text_v(cap));
    out.set("capabilities", std::move(list));
    codec::Value abi = codec::Value::map_v();
    abi.set("major", codec::Value::uint_v(PIME_ABI_MAJOR));
    abi.set("minor", codec::Value::uint_v(PIME_ABI_MINOR));
    out.set("abi", std::move(abi));
    out.set("host_caps", codec::Value::uint_v(host_caps));
    return out;
}

}  // namespace pime::cap
