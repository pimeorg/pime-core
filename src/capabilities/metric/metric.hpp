#ifndef PIME_CAPABILITIES_METRIC_HPP
#define PIME_CAPABILITIES_METRIC_HPP

#include <pime/abi.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "codec/cbor.hpp"

namespace pime::cap {

class Metrics {
public:
    void add(const std::string& alias, const std::string& name, int64_t delta);
    void set(const std::string& alias, const std::string& name, int64_t value);
    int64_t get(const std::string& alias, const std::string& name) const;
    codec::Value snapshot(const std::string& alias) const;
    codec::Value snapshot_all() const;

private:
    static std::string key(const std::string& alias, const std::string& name);
    mutable std::mutex mutex_;
    std::unordered_map<std::string, int64_t> values_;
};

}  // namespace pime::cap

#endif
