#ifndef PIME_CAPABILITIES_CONFIG_HPP
#define PIME_CAPABILITIES_CONFIG_HPP

#include <pime/abi.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "codec/cbor.hpp"

namespace pime::cap {

/* Immutable generation snapshot. Hot path lookups are read-only. */
class ConfigSnapshot {
public:
    ConfigSnapshot() = default;
    ConfigSnapshot(uint64_t id, std::map<std::string, codec::Value> global,
                   std::map<std::string, std::map<std::string, codec::Value>> plugins);

    uint64_t id() const { return id_; }

    bool get(const std::string& alias, const std::string& key,
             codec::Value* out) const;
    std::vector<std::string> list(const std::string& alias,
                                  const std::string& prefix) const;
    /* config map passed to a plugin call. */
    codec::Value for_plugin(const std::string& alias) const;

private:
    uint64_t id_ = 0;
    std::map<std::string, codec::Value> global_;
    std::map<std::string, std::map<std::string, codec::Value>> plugins_;
};

}  // namespace pime::cap

#endif
