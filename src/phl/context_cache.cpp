#include "context_cache.hpp"

#include <pime/hal.h>

#include "platform/platform.hpp"

namespace pime::phl {

std::string ContextCache::key_of(const std::string& alias, const std::string& name,
                                 ContextKind kind) {
    std::string key;
    key.reserve(alias.size() + name.size() + 8);
    key.append(alias);
    key.push_back('\0');
    key.append(name);
    key.push_back('\0');
    key.push_back(static_cast<char>(kind));
    return key;
}

void ContextCache::evict(std::shared_ptr<ContextEntry> entry) {
    if (entry->handle) handles_.erase(entry->handle);
    entries_.erase(key_of(entry->alias, entry->name, entry->kind));
}

void ContextCache::evict_expired_locked() {
    const uint64_t now = platform::monotonic_ms();
    std::vector<std::shared_ptr<ContextEntry>> dead;
    for (auto& kv : entries_) {
        auto& e = kv.second;
        if (e->refs == 0 && !e->pinned && e->ttl_ms > 0 &&
            now - e->last_used_ms > e->ttl_ms) {
            dead.push_back(e);
        }
    }
    for (auto& e : dead) evict(e);
}

pime_status ContextCache::acquire(const std::string& alias, const std::string& name,
                                  ContextKind kind, uint32_t schema,
                                  const std::string& code_hash, uint32_t flags,
                                  uint64_t* handle) {
    if (!handle || alias.empty() || name.empty()) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    evict_expired_locked();
    const std::string key = key_of(alias, name, kind);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        auto& e = it->second;
        if (e->schema != schema) return PIME_STALE;
        if (!code_hash.empty() && !e->code_hash.empty() &&
            e->code_hash != code_hash)
            return PIME_STALE;
        if (e->retired) return PIME_STALE;
        ++e->refs;
        e->last_used_ms = platform::monotonic_ms();
        if (flags & PIME_CTX_PIN) e->pinned = true;
        *handle = e->handle;
        return PIME_OK;
    }
    if (!(flags & PIME_CTX_CREATE)) return PIME_NOT_FOUND;
    auto e = std::make_shared<ContextEntry>();
    e->alias = alias;
    e->name = name;
    e->kind = kind;
    e->schema = schema;
    e->code_hash = code_hash;
    e->refs = 1;
    e->pinned = (flags & PIME_CTX_PIN) != 0;
    e->last_used_ms = platform::monotonic_ms();
    e->handle = handles_.insert(e);
    if (e->handle == 0) return PIME_NO_MEMORY;
    entries_[key] = e;
    *handle = e->handle;
    return PIME_OK;
}

pime_status ContextCache::release(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto e = handles_.get(handle, nullptr);
    if (!e) return PIME_STALE;
    if (e->refs == 0) return PIME_INVALID;
    --e->refs;
    e->last_used_ms = platform::monotonic_ms();
    if (e->refs == 0 && e->retired) evict(e);
    return PIME_OK;
}

pime_status ContextCache::pin(uint64_t handle, bool pinned) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto e = handles_.get(handle, nullptr);
    if (!e) return PIME_STALE;
    e->pinned = pinned;
    if (!pinned && e->retired && e->refs == 0) evict(e);
    return PIME_OK;
}

pime_status ContextCache::drop(const std::string& alias, const std::string& name,
                               ContextKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key_of(alias, name, kind));
    if (it == entries_.end()) return PIME_NOT_FOUND;
    if (it->second->refs > 0) return PIME_BUSY;
    evict(it->second);
    return PIME_OK;
}

pime_status ContextCache::info(const std::string& alias, const std::string& name,
                               ContextKind kind, ContextInfo* out) const {
    if (!out) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key_of(alias, name, kind));
    if (it == entries_.end()) return PIME_NOT_FOUND;
    const auto& e = it->second;
    out->kind = e->kind;
    out->size = e->size ? e->size : e->data.size();
    out->refs = e->refs;
    out->pinned = e->pinned;
    out->ttl_ms = e->ttl_ms;
    out->last_used_ms = e->last_used_ms;
    return PIME_OK;
}

pime_status ContextCache::set_data(uint64_t handle, const void* data, size_t len) {
    if (!data && len != 0) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    auto e = handles_.get(handle, nullptr);
    if (!e) return PIME_STALE;
    e->data.assign(static_cast<const uint8_t*>(data),
                   static_cast<const uint8_t*>(data) + len);
    e->size = len;
    return PIME_OK;
}

pime_status ContextCache::get_data(uint64_t handle, std::vector<uint8_t>* out) const {
    if (!out) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    auto e = handles_.get(handle, nullptr);
    if (!e) return PIME_STALE;
    *out = e->data;
    return PIME_OK;
}

pime_status ContextCache::set_resource(uint64_t handle,
                                       std::shared_ptr<void> resource,
                                       uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto e = handles_.get(handle, nullptr);
    if (!e) return PIME_STALE;
    e->resource = std::move(resource);
    e->size = size;
    return PIME_OK;
}

pime_status ContextCache::set_call(
    uint64_t handle,
    std::function<pime_status(const std::vector<uint8_t>&, std::vector<uint8_t>*)> call) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto e = handles_.get(handle, nullptr);
    if (!e) return PIME_STALE;
    e->call = std::move(call);
    return PIME_OK;
}

pime_status ContextCache::call(uint64_t handle, const std::vector<uint8_t>& input,
                               std::vector<uint8_t>* output) {
    if (!output) return PIME_INVALID;
    std::function<pime_status(const std::vector<uint8_t>&, std::vector<uint8_t>*)> fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto e = handles_.get(handle, nullptr);
        if (!e) return PIME_STALE;
        if (!e->call) return PIME_UNSUPPORTED;
        fn = e->call;
    }
    return fn(input, output);
}

void ContextCache::on_memory_pressure(uint32_t level) {
    if (level < PIME_MEMORY_WARNING) return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ContextEntry>> dead;
    for (auto& kv : entries_) {
        auto& e = kv.second;
        if (e->refs != 0 || e->pinned) continue;
        if (e->kind == ContextKind::Resource) continue;
        dead.push_back(e);
    }
    for (auto& e : dead) evict(e);
}

void ContextCache::on_generation_retire(const std::string& alias,
                                        const std::string& code_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ContextEntry>> dead;
    for (auto& kv : entries_) {
        auto& e = kv.second;
        if (e->alias != alias) continue;
        if (!code_hash.empty() && e->code_hash != code_hash) continue;
        if (e->kind == ContextKind::Data) continue;
        e->retired = true;
        if (e->refs == 0) dead.push_back(e);
    }
    for (auto& e : dead) evict(e);
}

void ContextCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    handles_.clear();
}

}  // namespace pime::phl
