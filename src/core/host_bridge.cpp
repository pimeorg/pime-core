#include <algorithm>
#include <cstring>
#include <limits>

#include "capabilities/audio/opus.hpp"
#include "capabilities/audio/pcm.hpp"
#include "capabilities/crypto/crypto.hpp"
#include "capabilities/time/time.hpp"
#include "core.hpp"
#include "phl/handle.hpp"
#include "platform/platform.hpp"
#include "util/result.hpp"

namespace pime::core {

namespace {

using codec::Value;

Value make_map() { return Value::map_v(); }
Value make_array() { return Value::array_v(); }

bool is_string(const Value* value) {
    return value && (value->is_text() || value->is_bytes());
}

std::string string_field(const Value& map, const char* key,
                         const std::string& fallback = std::string()) {
    const Value* value = map.find(key);
    return is_string(value) ? std::string(value->as_text()) : fallback;
}

std::string key_field(const Value& map) {
    const Value* value = map.find("key");
    if (!is_string(value)) value = map.find("k");
    return is_string(value) ? std::string(value->as_text()) : std::string();
}

uint64_t uint_field(const Value& map, const char* key, uint64_t fallback = 0) {
    const Value* value = map.find(key);
    return value && value->is_number() ? value->as_uint() : fallback;
}

int64_t int_field(const Value& map, const char* key, int64_t fallback = 0) {
    const Value* value = map.find(key);
    return value && value->is_number() ? value->as_int() : fallback;
}

bool bool_field(const Value& map, const char* key, bool fallback = false) {
    const Value* value = map.find(key);
    return value ? value->as_bool() : fallback;
}

std::vector<uint8_t> bytes_field(const Value& map, const char* key) {
    const Value* value = map.find(key);
    if (!is_string(value)) return {};
    const std::string_view view = value->as_text();
    return std::vector<uint8_t>(view.begin(), view.end());
}

Value bytes_value(const std::vector<uint8_t>& data) {
    return Value::bytes(std::string_view(
        data.empty() ? "" : reinterpret_cast<const char*>(data.data()),
        data.size()));
}

uint64_t handle_field(const Value& map) {
    const Value* value = map.find("handle");
    if (!value) value = map.find("h");
    return value && value->is_number() ? value->as_uint() : 0;
}

phl::ContextKind parse_kind(const Value& request) {
    const Value* value = request.find("kind");
    if (!value) return phl::ContextKind::Data;
    if (is_string(value)) {
        const std::string_view kind = value->as_text();
        if (kind == "instance") return phl::ContextKind::Instance;
        if (kind == "resource") return phl::ContextKind::Resource;
        return phl::ContextKind::Data;
    }
    const uint64_t kind = value->as_uint();
    if (kind == 1) return phl::ContextKind::Instance;
    if (kind == 2) return phl::ContextKind::Resource;
    return phl::ContextKind::Data;
}

phl::MessageMode parse_mode(const Value& request) {
    const Value* value = request.find("mode");
    if (!value) return phl::MessageMode::Inline;
    if (is_string(value)) {
        const std::string_view mode = value->as_text();
        if (mode == "buffer") return phl::MessageMode::Buffer;
        if (mode == "shm") return phl::MessageMode::Shm;
        return phl::MessageMode::Inline;
    }
    const uint64_t mode = value->as_uint();
    if (mode == 1) return phl::MessageMode::Buffer;
    if (mode == 2) return phl::MessageMode::Shm;
    return phl::MessageMode::Inline;
}

uint32_t mode_value(phl::MessageMode mode) {
    return static_cast<uint32_t>(mode);
}

Value tensor_list(const std::vector<codec::TensorSpec>& tensors) {
    Value list = make_array();
    for (const auto& tensor : tensors) {
        Value entry = make_map();
        entry.set("dtype", Value::text_v(tensor.dtype));
        Value shape = make_array();
        for (uint32_t dim : tensor.shape) shape.push(Value::uint_v(dim));
        entry.set("shape", std::move(shape));
        list.push(std::move(entry));
    }
    return list;
}

}  // namespace

pime_status Core::check(const host::HostCallContext& context,
                        const std::string& capability) {
    if (!context.caps || capability.empty()) return PIME_INVALID;
    if (context.secure) {
        if (capability.rfind("net.", 0) == 0) return PIME_DENIED;
        if (capability == "context.read" || capability == "surrounding.read" ||
            capability == "input.read")
            return PIME_DENIED;
    }
    return permission_.allowed(context.alias, capability) ? PIME_OK
                                                          : PIME_DENIED;
}

pime_status Core::invoke(const host::HostCallContext& context,
                         host::HostKind kind, uint8_t op, const uint8_t* in,
                         uint32_t in_len, std::vector<uint8_t>* out) {
    if (!out || !context.caps) return PIME_INVALID;
    Value request = make_map();
    if (in_len) {
        if (!in) return PIME_INVALID;
        pime_status status = codec::decode(in, in_len, &request);
        if (status != PIME_OK) return status;
        if (!request.is_map()) return PIME_INVALID;
    }

    auto reply = [out](pime_status status, Value result) -> pime_status {
        Value response = make_map();
        response.set("status", Value::uint_v(static_cast<uint64_t>(status)));
        if (status == PIME_OK) {
            response.set("result", std::move(result));
        } else {
            Value error = make_map();
            error.set("code", Value::uint_v(static_cast<uint64_t>(status)));
            error.set("message", Value::text_v(status_name(status)));
            response.set("error", std::move(error));
        }
        out->clear();
        codec::encode(response, out);
        return status;
    };
    auto deny = [&reply]() { return reply(PIME_DENIED, Value::null()); };
    auto allowed = [this, &context](const char* capability) {
        return permission_.allowed(context.alias, capability);
    };

    switch (kind) {
        case host::HostKind::Workspace: {
            if (!allowed("workspace")) return deny();
            if (!active_workspace_) return reply(PIME_NOT_FOUND, Value::null());
            if (op == static_cast<uint8_t>(host::WorkspaceOp::Get)) {
                const std::string key = key_field(request);
                if (key.empty()) return reply(PIME_INVALID, Value::null());
                Value value;
                const bool found =
                    key.rfind("plugin/", 0) == 0
                        ? active_workspace_->get(key, &value)
                        : active_workspace_->get_self(context.alias, key, &value);
                if (!found) return reply(PIME_NOT_FOUND, Value::null());
                return reply(PIME_OK, std::move(value));
            }
            if (op == static_cast<uint8_t>(host::WorkspaceOp::Set)) {
                const std::string key = key_field(request);
                const Value* value = request.find("value");
                if (!value) value = request.find("v");
                if (key.empty() || !value) return reply(PIME_INVALID, Value::null());
                pime_status status =
                    active_workspace_->set(context.alias, key, *value);
                return reply(status, Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Store: {
            if (!allowed("store.kv")) return deny();
            if (!store_) return reply(PIME_DENIED, Value::null());
            const auto store_op = static_cast<host::StoreOp>(op);
            if (store_op == host::StoreOp::Get) {
                std::string value;
                bool found = false;
                pime_status status =
                    store_->get(*context.caps, key_field(request), &value, &found);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("found", Value::boolean_v(found));
                result.set("value", Value::text_v(value));
                return reply(PIME_OK, std::move(result));
            }
            if (store_op == host::StoreOp::Set) {
                if (context.secure) return deny();
                const Value* value = request.find("value");
                if (!value) value = request.find("v");
                if (!value) return reply(PIME_INVALID, Value::null());
                pime_status status = store_->set(
                    *context.caps, key_field(request),
                    std::string(is_string(value) ? value->as_text() : ""));
                return reply(status, Value::map_v());
            }
            if (store_op == host::StoreOp::Del) {
                if (context.secure) return deny();
                bool deleted = false;
                pime_status status =
                    store_->del(*context.caps, key_field(request), &deleted);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("deleted", Value::boolean_v(deleted));
                return reply(PIME_OK, std::move(result));
            }
            if (store_op == host::StoreOp::Batch) {
                if (context.secure) return deny();
                std::vector<cap::StoreOp> operations;
                const Value* list = request.find("ops");
                if (!list || !list->is_array()) return reply(PIME_INVALID, Value::null());
                for (const auto& entry : list->array) {
                    if (!entry.is_map()) return reply(PIME_INVALID, Value::null());
                    cap::StoreOp store_op_entry;
                    const std::string name = string_field(entry, "op");
                    store_op_entry.kind = name == "del" ? cap::StoreOp::Kind::Del
                                                        : cap::StoreOp::Kind::Set;
                    store_op_entry.key = key_field(entry);
                    const Value* value = entry.find("value");
                    if (!value) value = entry.find("v");
                    if (is_string(value))
                        store_op_entry.value = std::string(value->as_text());
                    operations.push_back(std::move(store_op_entry));
                }
                pime_status status = store_->batch(*context.caps, operations);
                return reply(status, Value::map_v());
            }
            if (store_op == host::StoreOp::Scan) {
                std::vector<std::pair<std::string, std::string>> items;
                uint64_t limit = uint_field(request, "limit", 1000);
                if (limit == 0 || limit > 10000) limit = 10000;
                pime_status status = store_->scan(
                    *context.caps, string_field(request, "prefix"),
                    static_cast<uint32_t>(limit), &items);
                if (status != PIME_OK) return reply(status, Value::null());
                Value list = make_array();
                for (const auto& item : items) {
                    Value entry = make_map();
                    entry.set("k", Value::text_v(item.first));
                    entry.set("v", Value::text_v(item.second));
                    list.push(std::move(entry));
                }
                return reply(PIME_OK, std::move(list));
            }
            if (store_op == host::StoreOp::Quota) {
                uint64_t used = 0;
                uint64_t limit = 0;
                pime_status status = store_->quota(*context.caps, &used, &limit);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("used", Value::uint_v(used));
                result.set("limit", Value::uint_v(limit));
                return reply(PIME_OK, std::move(result));
            }
            if (store_op == host::StoreOp::Flush) {
                pime_status status = store_->flush(*context.caps);
                return reply(status, Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Asset: {
            if (!allowed("asset.read")) return deny();
            const auto asset_op = static_cast<host::AssetOp>(op);
            const std::string& root = context.caps->asset_dir;
            if (asset_op == host::AssetOp::List) {
                std::vector<std::string> names;
                pime_status status = assets_.list(root, &names);
                if (status != PIME_OK) return reply(status, Value::null());
                Value list = make_array();
                for (const auto& name : names) list.push(Value::text_v(name));
                return reply(PIME_OK, std::move(list));
            }
            if (asset_op == host::AssetOp::Open) {
                uint64_t handle = 0;
                pime_status status =
                    assets_.open(root, string_field(request, "path"), &handle);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (asset_op == host::AssetOp::Size) {
                uint64_t size = 0;
                pime_status status = assets_.size(handle_field(request), &size);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("size", Value::uint_v(size));
                return reply(PIME_OK, std::move(result));
            }
            if (asset_op == host::AssetOp::Read) {
                uint64_t offset = uint_field(request, "offset");
                uint64_t length = uint_field(request, "len", 65536);
                if (length == 0 || length > 4u * 1024u * 1024u)
                    return reply(PIME_INVALID, Value::null());
                std::vector<uint8_t> data(static_cast<size_t>(length));
                uint32_t out_len = 0;
                pime_status status = assets_.read(
                    handle_field(request), static_cast<uint32_t>(offset),
                    static_cast<uint32_t>(length), data.data(),
                    static_cast<uint32_t>(data.size()), &out_len);
                if (status != PIME_OK) return reply(status, Value::null());
                data.resize(out_len);
                return reply(PIME_OK, bytes_value(data));
            }
            if (asset_op == host::AssetOp::Map) {
                const void* ptr = nullptr;
                uint32_t size = 0;
                pime_status status = assets_.map(handle_field(request), &ptr, &size);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("size", Value::uint_v(size));
                return reply(PIME_OK, std::move(result));
            }
            if (asset_op == host::AssetOp::Close) {
                return reply(assets_.close(handle_field(request)), Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Config: {
            if (!allowed("config.read")) return deny();
            auto it = generations_.find(context.generation);
            if (it == generations_.end()) return reply(PIME_STALE, Value::null());
            const cap::ConfigSnapshot& snapshot = it->second->config;
            const auto config_op = static_cast<host::ConfigOp>(op);
            if (config_op == host::ConfigOp::Get) {
                Value value;
                const bool found =
                    snapshot.get(context.alias, key_field(request), &value);
                Value result = make_map();
                result.set("found", Value::boolean_v(found));
                if (found) result.set("value", std::move(value));
                return reply(PIME_OK, std::move(result));
            }
            if (config_op == host::ConfigOp::List) {
                Value list = make_array();
                for (const auto& key : snapshot.list(context.alias,
                                                      string_field(request, "prefix")))
                    list.push(Value::text_v(key));
                return reply(PIME_OK, std::move(list));
            }
            if (config_op == host::ConfigOp::Snapshot) {
                Value result = make_map();
                result.set("id", Value::uint_v(snapshot.id()));
                return reply(PIME_OK, std::move(result));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Ctx: {
            if (!allowed("ctx")) return deny();
            phl::ContextCache* cache = ctx_cache(context.session_id);
            const auto ctx_op = static_cast<host::CtxOp>(op);
            if (ctx_op == host::CtxOp::Acquire) {
                uint64_t flags = uint_field(request, "flags");
                if ((flags & PIME_CTX_SHARE) && !allowed("ctx.share")) return deny();
                uint64_t handle = 0;
                pime_status status = cache->acquire(
                    context.alias, string_field(request, "name"), parse_kind(request),
                    static_cast<uint32_t>(uint_field(request, "schema")),
                    context.caps->code_hash, static_cast<uint32_t>(flags),
                    &handle);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (ctx_op == host::CtxOp::Release)
                return reply(cache->release(handle_field(request)), Value::map_v());
            if (ctx_op == host::CtxOp::Pin) {
                return reply(cache->pin(handle_field(request),
                                        bool_field(request, "pin", true)),
                             Value::map_v());
            }
            if (ctx_op == host::CtxOp::Drop) {
                return reply(cache->drop(context.alias,
                                         string_field(request, "name"),
                                         parse_kind(request)),
                             Value::map_v());
            }
            if (ctx_op == host::CtxOp::Info) {
                phl::ContextInfo info;
                pime_status status =
                    cache->info(context.alias, string_field(request, "name"),
                                parse_kind(request), &info);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("kind", Value::uint_v(static_cast<uint64_t>(info.kind)));
                result.set("size", Value::uint_v(info.size));
                result.set("refs", Value::uint_v(info.refs));
                result.set("pinned", Value::boolean_v(info.pinned));
                result.set("ttl_ms", Value::uint_v(info.ttl_ms));
                result.set("last_used_ms", Value::uint_v(info.last_used_ms));
                return reply(PIME_OK, std::move(result));
            }
            if (ctx_op == host::CtxOp::Call) {
                std::vector<uint8_t> output;
                pime_status status =
                    cache->call(handle_field(request), bytes_field(request, "in"),
                                &output);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(output));
            }
            if (ctx_op == host::CtxOp::Set) {
                if (context.secure) return deny();
                std::vector<uint8_t> data = bytes_field(request, "data");
                pime_status status = cache->set_data(
                    handle_field(request), data.data(), data.size());
                return reply(status, Value::map_v());
            }
            if (ctx_op == host::CtxOp::Get) {
                std::vector<uint8_t> data;
                pime_status status = cache->get_data(handle_field(request), &data);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(data));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Channel: {
            if (!allowed("channel")) return deny();
            const auto channel_op = static_cast<host::ChannelOp>(op);
            if (channel_op == host::ChannelOp::Create) {
                auto channel = std::make_shared<phl::Channel>(
                    static_cast<uint32_t>(uint_field(request, "capacity", 0)));
                uint64_t handle = channels_.insert(channel);
                if (!handle) return reply(PIME_RESOURCE, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (channel_op == host::ChannelOp::Send) {
                auto channel = channels_.get(handle_field(request));
                if (!channel) return reply(PIME_STALE, Value::null());
                phl::Message message;
                message.mode = parse_mode(request);
                message.data = bytes_field(request, "data");
                message.handle = uint_field(request, "buffer");
                message.offset = static_cast<uint32_t>(uint_field(request, "offset"));
                message.length = static_cast<uint32_t>(uint_field(request, "len"));
                pime_status status =
                    channel->send(message,
                                  static_cast<uint32_t>(uint_field(request, "timeout_ms")));
                return reply(status, Value::map_v());
            }
            if (channel_op == host::ChannelOp::Recv) {
                auto channel = channels_.get(handle_field(request));
                if (!channel) return reply(PIME_STALE, Value::null());
                phl::Message message;
                pime_status status = channel->recv(
                    &message, static_cast<uint32_t>(
                                   uint_field(request, "timeout_ms",
                                              phl::kInfiniteMs)));
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("mode", Value::uint_v(mode_value(message.mode)));
                if (message.mode == phl::MessageMode::Inline)
                    result.set("data", bytes_value(message.data));
                else {
                    result.set("buffer", Value::uint_v(message.handle));
                    result.set("offset", Value::uint_v(message.offset));
                    result.set("len", Value::uint_v(message.length));
                }
                return reply(PIME_OK, std::move(result));
            }
            if (channel_op == host::ChannelOp::Close) {
                auto channel = channels_.get(handle_field(request));
                if (!channel) return reply(PIME_STALE, Value::null());
                pime_status status = channel->close(bool_field(request, "abort"));
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(channels_.erase(handle_field(request)), Value::map_v());
            }
            if (channel_op == host::ChannelOp::Depth) {
                auto channel = channels_.get(handle_field(request));
                if (!channel) return reply(PIME_STALE, Value::null());
                Value result = make_map();
                result.set("depth", Value::uint_v(channel->depth()));
                return reply(PIME_OK, std::move(result));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Sync: {
            if (!allowed("sync")) return deny();
            const auto sync_op = static_cast<host::SyncOp>(op);
            if (sync_op == host::SyncOp::MutexCreate) {
                auto mutex = std::make_shared<phl::Mutex>(
                    bool_field(request, "recursive"));
                phl::SyncRegistry::register_mutex(mutex);
                uint64_t handle = mutexes_.insert(mutex);
                if (!handle) return reply(PIME_RESOURCE, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (sync_op == host::SyncOp::MutexLock) {
                auto mutex = mutexes_.get(handle_field(request));
                if (!mutex) return reply(PIME_STALE, Value::null());
                return reply(mutex->lock(static_cast<uint32_t>(
                                 uint_field(request, "timeout_ms",
                                            phl::kInfiniteMs))),
                             Value::map_v());
            }
            if (sync_op == host::SyncOp::MutexTryLock) {
                auto mutex = mutexes_.get(handle_field(request));
                if (!mutex) return reply(PIME_STALE, Value::null());
                return reply(mutex->try_lock(), Value::map_v());
            }
            if (sync_op == host::SyncOp::MutexUnlock) {
                auto mutex = mutexes_.get(handle_field(request));
                if (!mutex) return reply(PIME_STALE, Value::null());
                return reply(mutex->unlock(), Value::map_v());
            }
            if (sync_op == host::SyncOp::CondvarCreate) {
                auto condvar = std::make_shared<phl::Condvar>();
                uint64_t handle = condvars_.insert(condvar);
                if (!handle) return reply(PIME_RESOURCE, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (sync_op == host::SyncOp::CondvarWait) {
                auto condvar = condvars_.get(handle_field(request));
                auto mutex = mutexes_.get(uint_field(request, "mutex"));
                if (!condvar || !mutex) return reply(PIME_STALE, Value::null());
                return reply(condvar->wait(mutex.get(),
                                           static_cast<uint32_t>(uint_field(
                                               request, "timeout_ms",
                                               phl::kInfiniteMs))),
                             Value::map_v());
            }
            if (sync_op == host::SyncOp::CondvarSignal ||
                sync_op == host::SyncOp::CondvarBroadcast) {
                auto condvar = condvars_.get(handle_field(request));
                if (!condvar) return reply(PIME_STALE, Value::null());
                if (sync_op == host::SyncOp::CondvarSignal)
                    condvar->signal();
                else
                    condvar->broadcast();
                return reply(PIME_OK, Value::map_v());
            }
            if (sync_op == host::SyncOp::Close) {
                const uint64_t handle = handle_field(request);
                if (condvars_.get(handle)) {
                    auto condvar = condvars_.get(handle);
                    condvar->close();
                    return reply(condvars_.erase(handle), Value::map_v());
                }
                auto mutex = mutexes_.get(handle);
                if (!mutex) return reply(PIME_NOT_FOUND, Value::null());
                return reply(mutexes_.erase(handle), Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Shm: {
            if (!allowed("shm")) return deny();
            const auto shm_op = static_cast<host::ShmOp>(op);
            if (shm_op == host::ShmOp::Create) {
                const uint64_t size = uint_field(request, "size");
                if (size == 0 || size > 256ull * 1024ull * 1024ull)
                    return reply(PIME_INVALID, Value::null());
                auto buffer = std::make_shared<phl::SharedBuffer>(
                    static_cast<size_t>(size), bool_field(request, "secure"));
                uint64_t handle = shared_buffers_.insert(buffer);
                if (!handle) return reply(PIME_RESOURCE, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (shm_op == host::ShmOp::Open) {
                auto buffer = shared_buffers_.get(handle_field(request));
                if (!buffer) return reply(PIME_STALE, Value::null());
                uint64_t handle = shared_buffers_.insert(buffer);
                if (!handle) return reply(PIME_RESOURCE, Value::null());
                Value result = make_map();
                result.set("handle", Value::uint_v(handle));
                return reply(PIME_OK, std::move(result));
            }
            if (shm_op == host::ShmOp::Read) {
                auto buffer = shared_buffers_.get(handle_field(request));
                if (!buffer) return reply(PIME_STALE, Value::null());
                uint64_t length = uint_field(request, "len", 65536);
                if (length == 0 || length > 16u * 1024u * 1024u)
                    return reply(PIME_INVALID, Value::null());
                std::vector<uint8_t> data(static_cast<size_t>(length));
                uint32_t out_len = 0;
                pime_status status = buffer->read(
                    static_cast<uint32_t>(uint_field(request, "offset")),
                    data.data(), static_cast<uint32_t>(data.size()), &out_len);
                if (status != PIME_OK) return reply(status, Value::null());
                data.resize(out_len);
                return reply(PIME_OK, bytes_value(data));
            }
            if (shm_op == host::ShmOp::Write) {
                auto buffer = shared_buffers_.get(handle_field(request));
                if (!buffer) return reply(PIME_STALE, Value::null());
                std::vector<uint8_t> data = bytes_field(request, "data");
                return reply(buffer->write(
                                 static_cast<uint32_t>(uint_field(request, "offset")),
                                 data.data(), static_cast<uint32_t>(data.size())),
                             Value::map_v());
            }
            if (shm_op == host::ShmOp::Resize) {
                auto buffer = shared_buffers_.get(handle_field(request));
                if (!buffer) return reply(PIME_STALE, Value::null());
                return reply(buffer->resize(static_cast<size_t>(
                                 uint_field(request, "size"))),
                             Value::map_v());
            }
            if (shm_op == host::ShmOp::Map) {
                auto buffer = shared_buffers_.get(handle_field(request));
                if (!buffer) return reply(PIME_STALE, Value::null());
                void* ptr = nullptr;
                uint32_t size = 0;
                pime_status status = buffer->map(&ptr, &size);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("size", Value::uint_v(size));
                return reply(PIME_OK, std::move(result));
            }
            if (shm_op == host::ShmOp::Unmap) {
                auto buffer = shared_buffers_.get(handle_field(request));
                if (!buffer) return reply(PIME_STALE, Value::null());
                return reply(buffer->unmap(), Value::map_v());
            }
            if (shm_op == host::ShmOp::Close) {
                return reply(shared_buffers_.erase(handle_field(request)),
                             Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Net: {
            if (context.secure) return deny();
            const auto net_op = static_cast<host::NetOp>(op);
            if (net_op == host::NetOp::Http) {
                if (!allowed("net.http")) return deny();
                if (bool_field(request, "insecure") &&
                    !allowed("net.insecure"))
                    return deny();
                uint64_t id = 0;
                pime_status status = net_->http(*context.caps, request, &id);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("request", Value::uint_v(id));
                return reply(PIME_OK, std::move(result));
            }
            if (net_op == host::NetOp::Result) {
                Value result;
                pime_status status =
                    net_->take_result(uint_field(request, "request"), &result);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, std::move(result));
            }
            if (net_op == host::NetOp::Open) {
                const std::string proto = string_field(request, "proto", "sse");
                const char* capability = "net.sse";
                if (proto == "ws") capability = "net.ws";
                if (proto == "wss") capability = "net.wss";
                if (!allowed(capability)) return deny();
                if (bool_field(request, "insecure") && !allowed("net.insecure"))
                    return deny();
                uint64_t id = 0;
                pime_status status = net_->open(
                    *context.caps, proto, string_field(request, "url"),
                    request.find("options") ? *request.find("options")
                                            : Value::map_v(),
                    &id);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("id", Value::uint_v(id));
                return reply(PIME_OK, std::move(result));
            }
            if (net_op == host::NetOp::Send) {
                std::vector<uint8_t> data = bytes_field(request, "data");
                return reply(net_->send(uint_field(request, "id"), data.data(),
                                        static_cast<uint32_t>(data.size())),
                             Value::map_v());
            }
            if (net_op == host::NetOp::Close) {
                return reply(net_->close(uint_field(request, "id")),
                             Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Crypto: {
            if (!allowed("crypto.*")) return deny();
            const auto crypto_op = static_cast<host::CryptoOp>(op);
            if (crypto_op == host::CryptoOp::Digest) {
                std::vector<uint8_t> data = bytes_field(request, "data");
                std::vector<uint8_t> out_data;
                pime_status status = cap::crypto::digest(
                    string_field(request, "alg", "sha256"), data.data(),
                    data.size(), &out_data);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(out_data));
            }
            if (crypto_op == host::CryptoOp::Mac) {
                std::vector<uint8_t> data = bytes_field(request, "data");
                std::vector<uint8_t> out_data;
                pime_status status = cap::crypto::mac(
                    string_field(request, "alg", "hmac-sha256"),
                    bytes_field(request, "key"), data.data(), data.size(),
                    &out_data);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(out_data));
            }
            if (crypto_op == host::CryptoOp::AeadEncrypt) {
                std::vector<uint8_t> data = bytes_field(request, "plaintext");
                if (!request.find("plaintext"))
                    data = bytes_field(request, "data");
                std::vector<uint8_t> ciphertext;
                std::vector<uint8_t> tag;
                pime_status status = cap::crypto::aead_encrypt(
                    string_field(request, "alg", "aes-256-gcm"),
                    bytes_field(request, "key"), bytes_field(request, "iv"),
                    bytes_field(request, "aad"), data.data(), data.size(),
                    &ciphertext, &tag);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("ciphertext", bytes_value(ciphertext));
                result.set("tag", bytes_value(tag));
                return reply(PIME_OK, std::move(result));
            }
            if (crypto_op == host::CryptoOp::AeadDecrypt) {
                std::vector<uint8_t> data = bytes_field(request, "ciphertext");
                std::vector<uint8_t> plaintext;
                pime_status status = cap::crypto::aead_decrypt(
                    string_field(request, "alg", "aes-256-gcm"),
                    bytes_field(request, "key"), bytes_field(request, "iv"),
                    bytes_field(request, "aad"), data.data(), data.size(),
                    bytes_field(request, "tag"), &plaintext);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(plaintext));
            }
            if (crypto_op == host::CryptoOp::Random) {
                uint64_t length = uint_field(request, "len", 32);
                if (length > 1024 * 1024) return reply(PIME_INVALID, Value::null());
                std::vector<uint8_t> data(static_cast<size_t>(length));
                pime_status status =
                    cap::crypto::random_bytes(data.data(), data.size());
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(data));
            }
            if (crypto_op == host::CryptoOp::Verify) {
                std::string pem = string_field(request, "key");
                if (pem.empty()) pem = string_field(request, "pem");
                std::vector<uint8_t> data = bytes_field(request, "data");
                std::vector<uint8_t> signature = bytes_field(request, "sig");
                pime_status status = cap::crypto::verify_signature(
                    pem, string_field(request, "alg"),
                    data.data(), data.size(), signature.data(), signature.size());
                if (status == PIME_DENIED) {
                    Value result = make_map();
                    result.set("ok", Value::boolean_v(false));
                    return reply(PIME_OK, std::move(result));
                }
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("ok", Value::boolean_v(true));
                return reply(PIME_OK, std::move(result));
            }
            if (crypto_op == host::CryptoOp::Sign) {
                std::string pem = string_field(request, "key");
                if (pem.empty()) pem = string_field(request, "pem");
                std::vector<uint8_t> data = bytes_field(request, "data");
                std::vector<uint8_t> signature;
                pime_status status = cap::crypto::sign_detached(
                    pem, string_field(request, "alg"),
                    data.data(), data.size(), &signature);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(signature));
            }
            if (crypto_op == host::CryptoOp::Kdf) {
                uint64_t length = uint_field(request, "len", 32);
                if (length == 0 || length > 1024 * 1024)
                    return reply(PIME_INVALID, Value::null());
                std::vector<uint8_t> out_key;
                pime_status status = cap::crypto::kdf_hkdf(
                    bytes_field(request, "key"), bytes_field(request, "salt"),
                    bytes_field(request, "info"), static_cast<size_t>(length),
                    &out_key);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(out_key));
            }
            if (crypto_op == host::CryptoOp::X509Verify) {
                pime_status status = cap::crypto::x509_verify(
                    string_field(request, "cert"), string_field(request, "ca"));
                if (status == PIME_DENIED) {
                    Value result = make_map();
                    result.set("ok", Value::boolean_v(false));
                    return reply(PIME_OK, std::move(result));
                }
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("ok", Value::boolean_v(true));
                return reply(PIME_OK, std::move(result));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Audio: {
            const auto audio_op = static_cast<host::AudioOp>(op);
            if (audio_op == host::AudioOp::CaptureStart) {
                if (!allowed("audio.capture")) return deny();
                if (!(context.host_caps & PIME_HOST_AUDIO_CAPTURE))
                    return reply(PIME_UNSUPPORTED, Value::null());
                if (context.secure) return deny();
                cap::PcmFormat format;
                format.rate = static_cast<uint32_t>(uint_field(request, "rate", 16000));
                format.channels =
                    static_cast<uint32_t>(uint_field(request, "channels", 1));
                format.format = static_cast<uint32_t>(
                    uint_field(request, "format", PIME_AUDIO_S16LE));
                if (!cap::pcm_format_valid(format))
                    return reply(PIME_INVALID, Value::null());
                uint32_t stream = 0;
                pime_status status =
                    audio_.start_capture(context.session_id, format, &stream);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("stream", Value::uint_v(stream));
                return reply(PIME_OK, std::move(result));
            }
            if (audio_op == host::AudioOp::CaptureStop)
                return reply(audio_.stop_capture(context.session_id), Value::map_v());
            if (audio_op == host::AudioOp::CaptureRead) {
                if (!allowed("audio.capture")) return deny();
                if (context.secure) return deny();
                cap::AudioFrameData frame;
                pime_status status = audio_.read_capture(
                    context.session_id, &frame,
                    static_cast<uint32_t>(uint_field(request, "timeout_ms")));
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("rate", Value::uint_v(frame.format.rate));
                result.set("channels", Value::uint_v(frame.format.channels));
                result.set("format", Value::uint_v(frame.format.format));
                result.set("time_ms", Value::uint_v(frame.time_ms));
                result.set("data", bytes_value(frame.data));
                return reply(PIME_OK, std::move(result));
            }
            if (audio_op == host::AudioOp::PlaybackStart) {
                if (!allowed("audio.playback")) return deny();
                if (!(context.host_caps & PIME_HOST_AUDIO_PLAYBACK))
                    return reply(PIME_UNSUPPORTED, Value::null());
                cap::PcmFormat format;
                format.rate = static_cast<uint32_t>(uint_field(request, "rate", 16000));
                format.channels =
                    static_cast<uint32_t>(uint_field(request, "channels", 1));
                format.format = static_cast<uint32_t>(
                    uint_field(request, "format", PIME_AUDIO_S16LE));
                if (!cap::pcm_format_valid(format))
                    return reply(PIME_INVALID, Value::null());
                uint32_t stream = 0;
                pime_status status =
                    audio_.start_playback(context.session_id, format, &stream);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("stream", Value::uint_v(stream));
                return reply(PIME_OK, std::move(result));
            }
            if (audio_op == host::AudioOp::PlaybackStop)
                return reply(audio_.stop_playback(context.session_id), Value::map_v());
            if (audio_op == host::AudioOp::PlaybackWrite) {
                if (!allowed("audio.playback")) return deny();
                if (context.secure) return deny();
                cap::AudioFrameData frame;
                frame.format.rate =
                    static_cast<uint32_t>(uint_field(request, "rate", 16000));
                frame.format.channels =
                    static_cast<uint32_t>(uint_field(request, "channels", 1));
                frame.format.format = static_cast<uint32_t>(
                    uint_field(request, "format", PIME_AUDIO_S16LE));
                frame.time_ms = uint_field(request, "time_ms");
                frame.data = bytes_field(request, "data");
                return reply(audio_.push_playback(context.session_id, frame),
                             Value::map_v());
            }
            if (audio_op == host::AudioOp::PcmConvert) {
                if (!allowed("audio.pcm")) return deny();
                cap::PcmFormat from;
                from.rate = static_cast<uint32_t>(uint_field(request, "from_rate", 16000));
                from.channels =
                    static_cast<uint32_t>(uint_field(request, "from_channels", 1));
                from.format = static_cast<uint32_t>(
                    uint_field(request, "from_format", PIME_AUDIO_S16LE));
                cap::PcmFormat to;
                to.rate = static_cast<uint32_t>(uint_field(request, "to_rate", 16000));
                to.channels =
                    static_cast<uint32_t>(uint_field(request, "to_channels", 1));
                to.format = static_cast<uint32_t>(
                    uint_field(request, "to_format", PIME_AUDIO_S16LE));
                std::vector<uint8_t> data = bytes_field(request, "data");
                std::vector<uint8_t> output;
                pime_status status = cap::pcm_convert(
                    data.data(), static_cast<uint32_t>(data.size()), from, to,
                    static_cast<float>(int_field(request, "gain", 100) / 100.0),
                    &output);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(output));
            }
            if (audio_op == host::AudioOp::OpusEncode) {
                if (!allowed("audio.opus")) return deny();
                cap::PcmFormat format;
                format.rate = static_cast<uint32_t>(uint_field(request, "rate", 16000));
                format.channels =
                    static_cast<uint32_t>(uint_field(request, "channels", 1));
                format.format = static_cast<uint32_t>(
                    uint_field(request, "format", PIME_AUDIO_S16LE));
                std::vector<uint8_t> data = bytes_field(request, "data");
                std::vector<std::vector<uint8_t>> packets;
                pime_status status = cap::opus_encode(
                    data.data(), static_cast<uint32_t>(data.size()), format,
                    static_cast<uint32_t>(uint_field(request, "bitrate", 24000)),
                    bool_field(request, "fec"), &packets);
                if (status != PIME_OK) return reply(status, Value::null());
                Value list = make_array();
                for (const auto& packet : packets) list.push(bytes_value(packet));
                return reply(PIME_OK, std::move(list));
            }
            if (audio_op == host::AudioOp::OpusDecode) {
                if (!allowed("audio.opus")) return deny();
                std::vector<uint8_t> packet = bytes_field(request, "data");
                std::vector<uint8_t> pcm;
                pime_status status = cap::opus_decode(
                    packet.data(), static_cast<uint32_t>(packet.size()),
                    static_cast<uint32_t>(uint_field(request, "rate", 16000)),
                    static_cast<uint32_t>(uint_field(request, "channels", 1)),
                    bool_field(request, "plc"), &pcm);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(pcm));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Ml: {
            if (!allowed("ml.infer")) return deny();
            const auto ml_op = static_cast<host::MlOp>(op);
            if (ml_op == host::MlOp::Load) {
                if (!context.caps->manifest)
                    return reply(PIME_INVALID, Value::null());
                const std::string model_id = string_field(request, "model");
                const codec::ModelSpec* model = nullptr;
                for (const auto& spec : context.caps->manifest->models) {
                    if (spec.id == model_id) {
                        model = &spec;
                        break;
                    }
                }
                if (!model) return reply(PIME_NOT_FOUND, Value::null());
                const std::string engine = string_field(
                    request, "engine", model->engine);
                if (engine.empty()) return reply(PIME_UNSUPPORTED, Value::null());
                const std::string path =
                    platform::path_join(context.caps->asset_dir, model->path);
                if (!platform::path_exists(path))
                    return reply(PIME_NOT_FOUND, Value::null());
                Value desc = make_map();
                desc.set("model", Value::text_v(model->id));
                desc.set("path", Value::text_v(path));
                desc.set("format", Value::text_v(model->format));
                desc.set("hash", Value::text_v(model->hash));
                desc.set("input", tensor_list(model->input));
                desc.set("output", tensor_list(model->output));
                if (const Value* options = request.find("options"))
                    desc.set("options", *options);
                uint64_t session = 0;
                pime_status status =
                    ml_->load(*context.caps, engine, desc, &session);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("session", Value::uint_v(session));
                return reply(PIME_OK, std::move(result));
            }
            if (ml_op == host::MlOp::Run) {
                std::vector<uint8_t> output;
                pime_status status = ml_->run(
                    uint_field(request, "session"), bytes_field(request, "in"),
                    &output);
                if (status != PIME_OK) return reply(status, Value::null());
                return reply(PIME_OK, bytes_value(output));
            }
            if (ml_op == host::MlOp::RunAsync) {
                uint64_t job = 0;
                pime_status status = ml_->run_async(
                    uint_field(request, "session"), bytes_field(request, "in"),
                    &job);
                if (status != PIME_OK) return reply(status, Value::null());
                Value result = make_map();
                result.set("job", Value::uint_v(job));
                return reply(PIME_OK, std::move(result));
            }
            if (ml_op == host::MlOp::Cancel) {
                return reply(ml_->cancel(uint_field(request, "job")),
                             Value::map_v());
            }
            if (ml_op == host::MlOp::Free) {
                return reply(ml_->free_session(uint_field(request, "session")),
                             Value::map_v());
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Time: {
            if (!allowed("time")) return deny();
            uint64_t kind = uint_field(request, "kind");
            if (kind > 2) return reply(PIME_INVALID, Value::null());
            Value result = make_map();
            result.set("value", Value::uint_v(cap::time_query(
                                   static_cast<uint32_t>(kind))));
            return reply(PIME_OK, std::move(result));
        }
        case host::HostKind::Timer: {
            if (!allowed("timer")) return deny();
            const auto timer_op = static_cast<host::TimerOp>(op);
            if (timer_op == host::TimerOp::Cancel) {
                return reply(timers_.cancel(
                                 static_cast<uint32_t>(uint_field(request, "id"))),
                             Value::map_v());
            }
            std::string event_id = string_field(request, "event_id");
            if (event_id.empty())
                event_id = std::to_string(uint_field(request, "id"));
            if (event_id.empty()) return reply(PIME_INVALID, Value::null());
            uint32_t delay = static_cast<uint32_t>(uint_field(request, "delay_ms"));
            uint32_t repeat = 0;
            if (timer_op == host::TimerOp::Every) {
                repeat = static_cast<uint32_t>(
                    uint_field(request, "interval_ms", delay));
                delay = repeat;
            }
            uint32_t timer_id = 0;
            pime_status status = timers_.add(
                context.alias, context.generation, delay, repeat, event_id,
                &timer_id);
            if (status != PIME_OK) return reply(status, Value::null());
            Value result = make_map();
            result.set("id", Value::uint_v(timer_id));
            return reply(PIME_OK, std::move(result));
        }
        case host::HostKind::Caps: {
            if (!allowed("caps.query")) return deny();
            return reply(PIME_OK, cap::caps_query(permission_, context.alias,
                                                  context.host_caps));
        }
        case host::HostKind::Thread: {
            if (!allowed("thread")) return deny();
            const auto thread_op = static_cast<host::ThreadOp>(op);
            if (thread_op == host::ThreadOp::Create) {
                if (!context.instance)
                    return reply(PIME_UNSUPPORTED, Value::null());
                const std::string entry = string_field(request, "entry");
                if (entry.empty()) return reply(PIME_INVALID, Value::null());
                std::string error;
                phl::PluginThread::Entry function =
                    context.instance->thread_entry(entry, &error);
                if (!function)
                    return reply(PIME_UNSUPPORTED, Value::null());
                phl::ThreadDesc desc;
                desc.alias = context.alias;
                desc.entry = entry;
                desc.name = string_field(request, "name", entry);
                desc.stack_kb =
                    static_cast<uint32_t>(uint_field(request, "stack_kb"));
                desc.priority =
                    static_cast<uint32_t>(uint_field(request, "priority", 1));
                if (desc.priority > 2) return reply(PIME_INVALID, Value::null());
                desc.max_frame_bytes = context.caps->limits.max_frame_bytes;
                desc.generation = context.generation;
                auto thread = std::make_shared<phl::PluginThread>(
                    std::move(desc), std::move(function));
                pime_status status = thread->start();
                if (status != PIME_OK) return reply(status, Value::null());
                uint32_t id = 0;
                {
                    std::lock_guard<std::mutex> lock(thread_mutex_);
                    if (next_thread_id_ == 0)
                        return reply(PIME_RESOURCE, Value::null());
                    id = next_thread_id_++;
                    thread->set_id(id);
                    threads_[id] = thread;
                }
                Value result = make_map();
                result.set("tid", Value::uint_v(id));
                return reply(PIME_OK, std::move(result));
            }
            std::shared_ptr<phl::PluginThread> thread;
            {
                std::lock_guard<std::mutex> lock(thread_mutex_);
                auto it = threads_.find(
                    static_cast<uint32_t>(uint_field(request, "tid")));
                if (it != threads_.end()) thread = it->second;
            }
            if (!thread) return reply(PIME_STALE, Value::null());
            if (thread_op == host::ThreadOp::Post) {
                std::vector<uint8_t> data = bytes_field(request, "data");
                return reply(thread->post(data.data(),
                                          static_cast<uint32_t>(data.size()),
                                          static_cast<uint32_t>(uint_field(
                                              request, "timeout_ms"))),
                             Value::map_v());
            }
            if (thread_op == host::ThreadOp::Recv) {
                std::vector<uint8_t> data(thread->desc().max_frame_bytes);
                uint32_t out_len = 0;
                pime_status status = thread->recv(
                    data.data(), static_cast<uint32_t>(data.size()), &out_len,
                    static_cast<uint32_t>(uint_field(request, "timeout_ms",
                                                     phl::kInfiniteMs)));
                if (status != PIME_OK) return reply(status, Value::null());
                data.resize(out_len);
                return reply(PIME_OK, bytes_value(data));
            }
            if (thread_op == host::ThreadOp::Park) {
                return reply(thread->park(static_cast<uint32_t>(
                                 uint_field(request, "timeout_ms",
                                            phl::kInfiniteMs))),
                             Value::map_v());
            }
            if (thread_op == host::ThreadOp::Unpark) {
                thread->unpark();
                return reply(PIME_OK, Value::map_v());
            }
            if (thread_op == host::ThreadOp::Close) {
                return reply(thread->close(), Value::map_v());
            }
            if (thread_op == host::ThreadOp::Join) {
                pime_status status = thread->close();
                if (status == PIME_OK)
                    status = thread->join(static_cast<uint32_t>(
                        uint_field(request, "timeout_ms", phl::kInfiniteMs)));
                if (status != PIME_OK) return reply(status, Value::null());
                std::lock_guard<std::mutex> lock(thread_mutex_);
                threads_.erase(static_cast<uint32_t>(uint_field(request, "tid")));
                return reply(PIME_OK, Value::map_v());
            }
            if (thread_op == host::ThreadOp::Info) {
                Value result = make_map();
                result.set("running", Value::boolean_v(thread->running()));
                result.set("closing", Value::boolean_v(thread->closing()));
                result.set("depth", Value::uint_v(thread->depth()));
                result.set("priority",
                           Value::uint_v(thread->desc().priority));
                result.set("name", Value::text_v(thread->desc().name));
                return reply(PIME_OK, std::move(result));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Log: {
            if (!allowed("log")) return deny();
            const uint64_t level = uint_field(request, "level", PIME_LOG_INFO);
            std::string tag = string_field(request, "tag");
            std::string message = string_field(request, "message");
            if (context.secure) message.clear();
            if (context.caps)
                log_.plugin(*context.caps,
                            static_cast<pime_log_level>(
                                std::min<uint64_t>(level, PIME_LOG_TRACE)),
                            tag.c_str(), message.c_str());
            return reply(PIME_OK, Value::map_v());
        }
        case host::HostKind::Metric: {
            if (!allowed("metric")) return deny();
            const auto metric_op = static_cast<host::MetricOp>(op);
            const std::string name = string_field(request, "name");
            if (metric_op == host::MetricOp::Add) {
                metrics_.add(context.alias, name, int_field(request, "value"));
                return reply(PIME_OK, Value::map_v());
            }
            if (metric_op == host::MetricOp::Set) {
                metrics_.set(context.alias, name, int_field(request, "value"));
                return reply(PIME_OK, Value::map_v());
            }
            if (metric_op == host::MetricOp::Get) {
                Value result = make_map();
                result.set("value",
                           Value::int_v(metrics_.get(context.alias, name)));
                return reply(PIME_OK, std::move(result));
            }
            if (metric_op == host::MetricOp::Snapshot) {
                return reply(PIME_OK, metrics_.snapshot(context.alias));
            }
            return reply(PIME_UNSUPPORTED, Value::null());
        }
        case host::HostKind::Random: {
            if (!allowed("random")) return deny();
            uint64_t length = uint_field(request, "len", 32);
            if (length > 1024 * 1024) return reply(PIME_INVALID, Value::null());
            std::vector<uint8_t> data(static_cast<size_t>(length));
            pime_status status =
                platform::random_bytes(data.data(), data.size());
            if (status != PIME_OK) return reply(status, Value::null());
            return reply(PIME_OK, bytes_value(data));
        }
        default:
            return reply(PIME_UNSUPPORTED, Value::null());
    }
}

}  // namespace pime::core
