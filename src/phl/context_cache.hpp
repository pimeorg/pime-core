#ifndef PIME_PHL_CONTEXT_CACHE_HPP
#define PIME_PHL_CONTEXT_CACHE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "handle.hpp"

namespace pime::phl {

enum class ContextKind : uint32_t { Data = 0, Instance = 1, Resource = 2 };

/* acquire flags */
#define PIME_CTX_CREATE 1u
#define PIME_CTX_REQUIRED 2u
#define PIME_CTX_PIN 4u
#define PIME_CTX_SHARE 8u

struct ContextInfo {
    ContextKind kind = ContextKind::Data;
    uint64_t size = 0;
    uint32_t refs = 0;
    bool pinned = false;
    uint64_t ttl_ms = 0;
    uint64_t last_used_ms = 0;
};

struct ContextEntry {
    std::string alias;
    std::string name;
    ContextKind kind = ContextKind::Data;
    uint32_t schema = 0;
    std::string code_hash;
    uint32_t refs = 0;
    bool pinned = false;
    bool retired = false;
    uint64_t ttl_ms = 300000;
    uint64_t last_used_ms = 0;
    uint64_t size = 0;
    uint64_t handle = 0;
    std::vector<uint8_t> data;
    std::shared_ptr<void> resource;
    std::function<pime_status(const std::vector<uint8_t>&, std::vector<uint8_t>*)> call;
};

class ContextCache {
public:
    ContextCache() = default;

    pime_status acquire(const std::string& alias, const std::string& name,
                        ContextKind kind, uint32_t schema,
                        const std::string& code_hash, uint32_t flags,
                        uint64_t* handle);
    pime_status release(uint64_t handle);
    pime_status pin(uint64_t handle, bool pinned);
    pime_status drop(const std::string& alias, const std::string& name,
                     ContextKind kind);
    pime_status info(const std::string& alias, const std::string& name,
                     ContextKind kind, ContextInfo* out) const;

    pime_status set_data(uint64_t handle, const void* data, size_t len);
    pime_status get_data(uint64_t handle, std::vector<uint8_t>* out) const;
    pime_status set_resource(uint64_t handle, std::shared_ptr<void> resource,
                             uint64_t size);
    pime_status set_call(uint64_t handle,
                         std::function<pime_status(const std::vector<uint8_t>&,
                                                   std::vector<uint8_t>*)> call);
    pime_status call(uint64_t handle, const std::vector<uint8_t>& input,
                     std::vector<uint8_t>* output);

    /* warning/critical: evict unpinned, unreferenced DATA and INSTANCE. */
    void on_memory_pressure(uint32_t level);
    /* INSTANCE stops taking new leases; DATA follows schema/code hash rules. */
    void on_generation_retire(const std::string& alias,
                              const std::string& code_hash);
    void clear();

private:
    static std::string key_of(const std::string& alias, const std::string& name,
                              ContextKind kind);
    void evict(std::shared_ptr<ContextEntry> entry);
    void evict_expired_locked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ContextEntry>> entries_;
    HandleRegistry<ContextEntry> handles_;
};

}  // namespace pime::phl

#endif
