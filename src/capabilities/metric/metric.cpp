#include "metric.hpp"

namespace pime::cap {

std::string Metrics::key(const std::string& alias, const std::string& name) {
    return alias + "/" + name;
}

void Metrics::add(const std::string& alias, const std::string& name, int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key(alias, name)] += delta;
}

void Metrics::set(const std::string& alias, const std::string& name, int64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key(alias, name)] = value;
}

int64_t Metrics::get(const std::string& alias, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key(alias, name));
    return it == values_.end() ? 0 : it->second;
}

codec::Value Metrics::snapshot(const std::string& alias) const {
    std::lock_guard<std::mutex> lock(mutex_);
    codec::Value out = codec::Value::map_v();
    const std::string prefix = alias + "/";
    for (const auto& kv : values_) {
        if (kv.first.rfind(prefix, 0) == 0) {
            out.set(kv.first.substr(prefix.size()), codec::Value::int_v(kv.second));
        }
    }
    return out;
}

codec::Value Metrics::snapshot_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    codec::Value out = codec::Value::map_v();
    for (const auto& kv : values_) {
        out.set(kv.first, codec::Value::int_v(kv.second));
    }
    return out;
}

}  // namespace pime::cap
