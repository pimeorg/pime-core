#ifndef PIME_CAPABILITIES_STORE_HPP
#define PIME_CAPABILITIES_STORE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cap_context.hpp"

namespace pime::cap {

struct StoreOp {
    enum class Kind { Set, Del } kind = Kind::Set;
    std::string key;
    std::string value;
};

/* Per-plugin encrypted KV. Persisted with AES-256-GCM; no plaintext fallback. */
class Store {
public:
    Store(std::string data_dir, std::vector<uint8_t> key);

    pime_status get(const PluginCapContext& ctx, const std::string& key,
                    std::string* value, bool* found);
    pime_status set(const PluginCapContext& ctx, const std::string& key,
                    std::string value);
    pime_status del(const PluginCapContext& ctx, const std::string& key,
                    bool* deleted);
    pime_status batch(const PluginCapContext& ctx, const std::vector<StoreOp>& ops);
    pime_status scan(const PluginCapContext& ctx, const std::string& prefix,
                     uint32_t limit,
                     std::vector<std::pair<std::string, std::string>>* out);
    pime_status quota(const PluginCapContext& ctx, uint64_t* used,
                      uint64_t* limit);
    pime_status flush(const PluginCapContext& ctx);
    pime_status flush_all();

private:
    struct Namespace {
        std::map<std::string, std::string> data;
        uint32_t schema = 0;
        bool loaded = false;
        bool dirty = false;
    };

    std::string file_for(const std::string& alias) const;
    Namespace* load_locked(const PluginCapContext& ctx);
    pime_status save_locked(const PluginCapContext& ctx, Namespace& ns);
    uint64_t usage(const Namespace& ns) const;

    std::mutex mutex_;
    std::string data_dir_;
    std::vector<uint8_t> key_;
    std::unordered_map<std::string, Namespace> namespaces_;
};

}  // namespace pime::cap

#endif
