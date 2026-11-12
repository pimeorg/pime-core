#include "config.hpp"

#include "util/string_util.hpp"

namespace pime::cap {

ConfigSnapshot::ConfigSnapshot(
    uint64_t id, std::map<std::string, codec::Value> global,
    std::map<std::string, std::map<std::string, codec::Value>> plugins)
    : id_(id), global_(std::move(global)), plugins_(std::move(plugins)) {}

bool ConfigSnapshot::get(const std::string& alias, const std::string& key,
                         codec::Value* out) const {
    if (!out) return false;
    auto pit = plugins_.find(alias);
    if (pit != plugins_.end()) {
        auto it = pit->second.find(key);
        if (it != pit->second.end()) {
            *out = it->second;
            return true;
        }
    }
    auto it = global_.find(key);
    if (it == global_.end()) return false;
    *out = it->second;
    return true;
}

std::vector<std::string> ConfigSnapshot::list(const std::string& alias,
                                              const std::string& prefix) const {
    std::vector<std::string> keys;
    auto pit = plugins_.find(alias);
    if (pit != plugins_.end()) {
        for (const auto& kv : pit->second) {
            if (prefix.empty() || kv.first.rfind(prefix, 0) == 0)
                keys.push_back(kv.first);
        }
    }
    for (const auto& kv : global_) {
        if (prefix.empty() || kv.first.rfind(prefix, 0) == 0)
            keys.push_back(kv.first);
    }
    return keys;
}

codec::Value ConfigSnapshot::for_plugin(const std::string& alias) const {
    codec::Value map = codec::Value::map_v();
    map.set("snapshot", codec::Value::uint_v(id_));
    for (const auto& kv : global_) map.set(kv.first, kv.second);
    auto pit = plugins_.find(alias);
    if (pit != plugins_.end()) {
        for (const auto& kv : pit->second) map.set(kv.first, kv.second);
    }
    return map;
}

}  // namespace pime::cap
