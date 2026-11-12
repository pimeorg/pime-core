#ifndef PIME_CAPABILITIES_WORKSPACE_HPP
#define PIME_CAPABILITIES_WORKSPACE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <map>
#include <string>

#include "codec/cbor.hpp"

namespace pime::cap {

/* Shared key/value area for one chain invocation. Writes stage into a delta
 * and become visible to downstream plugins only after commit(). */
class Workspace {
public:
    explicit Workspace(uint32_t max_bytes = 256 * 1024);

    void reset();
    pime_status set(const std::string& alias, const std::string& key,
                    const codec::Value& value);
    bool get(const std::string& key, codec::Value* out) const;
    bool get_self(const std::string& alias, const std::string& key,
                  codec::Value* out) const;
    codec::Value delta() const;
    pime_status commit();
    void discard();
    codec::Value snapshot() const;

    static std::string full_key(const std::string& alias, const std::string& key);
    static bool valid_full_key(const std::string& key, std::string* alias);

private:
    uint64_t value_size(const std::string& key, const codec::Value& value) const;
    uint32_t max_bytes_;
    uint64_t used_ = 0;
    std::map<std::string, codec::Value> values_;
    std::map<std::string, codec::Value> staged_;
};

}  // namespace pime::cap

#endif
