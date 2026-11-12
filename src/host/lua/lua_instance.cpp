/*
 * Lua 5.4 plugin runtime. Third-party headers are confined to this directory.
 *
 * Lua tables map one-to-one to CBOR maps/arrays; capability calls go through
 * the same host bridge envelope as the WASM and native runtimes. The state is
 * created with a bounded allocator and an instruction-count hook.
 */
#include "plugin_instance.hpp"

#include <pime/hal.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "capabilities/cap_context.hpp"
#include "codec/cbor.hpp"
#include "platform/platform.hpp"
#include "phl/context_cache.hpp"
#include "util/result.hpp"

namespace pime::host {

namespace {

using codec::Value;

constexpr const char* kSelf = "pime.self";
constexpr uint32_t kHookInstructions = 10000;

class LuaInstance : public PluginInstance {
public:
    LuaInstance(std::string dir, const cap::PluginCapContext* caps,
                HostBridge* bridge, std::string alias, uint64_t generation,
                uint32_t memory_mb, uint64_t instructions)
        : dir_(std::move(dir)),
          caps_(caps),
          bridge_(bridge),
          alias_(std::move(alias)),
          generation_(generation),
          memory_limit_(static_cast<size_t>(memory_mb) * 1024u * 1024u),
          instruction_budget_(instructions) {}

    ~LuaInstance() override { shutdown(); }

    pime_status init(const std::vector<uint8_t>& config) override {
        state_ = lua_newstate(&LuaInstance::allocate, this);
        if (!state_) return PIME_NO_MEMORY;
        luaL_openlibs(state_);

        /* Remove anything that touches the filesystem, processes or the
         * package loader. */
        for (const char* name : {"dofile", "loadfile", "load", "require",
                                 "collectgarbage", "io", "os", "package",
                                 "debug"}) {
            lua_pushnil(state_);
            lua_setglobal(state_, name);
        }

        lua_pushlightuserdata(state_, this);
        lua_setfield(state_, LUA_REGISTRYINDEX, kSelf);
        register_api(state_);

        lua_sethook(state_, &LuaInstance::hook, LUA_MASKCOUNT,
                    kHookInstructions);

        const std::string path = platform::path_join(dir_, "plugin.lua");
        std::string text;
        if (platform::read_text_file(path, &text) != PIME_OK)
            return PIME_NOT_FOUND;
        if (luaL_loadbuffer(state_, text.data(), text.size(), "plugin.lua") !=
            LUA_OK) {
            error_ = lua_tostring(state_, -1);
            lua_pop(state_, 1);
            return PIME_CORRUPT;
        }
        if (!protected_call(0, 0)) return PIME_CORRUPT;

        Value config_value;
        if (codec::decode(config.data(), config.size(), &config_value) ==
            PIME_OK) {
            push_value(state_, config_value);
        } else {
            lua_newtable(state_);
        }
        lua_setglobal(state_, "pime_config");
        lua_getglobal(state_, "pime");
        if (!lua_istable(state_, -1)) {
            lua_pop(state_, 1);
            return PIME_CORRUPT;
        }
        lua_getfield(state_, -1, "init");
        if (lua_isfunction(state_, -1)) {
            lua_remove(state_, -2);
            lua_getglobal(state_, "pime_config");
            if (!protected_call(1, 0)) return PIME_CORRUPT;
        } else {
            lua_pop(state_, 2);
        }
        return PIME_OK;
    }

    pime_status process(const uint8_t* in, uint32_t in_len,
                        std::vector<uint8_t>* out) override {
        return call_output("process", in, in_len, out);
    }

    pime_status on_event(const uint8_t* evt, uint32_t evt_len,
                         std::vector<uint8_t>* out) override {
        return call_output("on_event", evt, evt_len, out);
    }

    void set_session(uint64_t session_id, bool secure,
                     uint32_t host_caps) override {
        session_id_ = session_id;
        secure_ = secure;
        host_caps_ = host_caps;
    }

    void shutdown() override {
        if (!state_) return;
        lua_getglobal(state_, "pime");
        if (lua_istable(state_, -1)) {
            lua_getfield(state_, -1, "shutdown");
            if (lua_isfunction(state_, -1)) {
                lua_remove(state_, -2);
                protected_call(0, 0);
            } else {
                lua_pop(state_, 2);
            }
        } else {
            lua_pop(state_, 1);
        }
        lua_close(state_);
        state_ = nullptr;
    }

    bool alive() const override { return state_ != nullptr; }

    HostCallContext context() const {
        HostCallContext call;
        call.alias = alias_;
        call.generation = generation_;
        call.session_id = session_id_;
        call.secure = secure_;
        call.host_caps = host_caps_;
        call.caps = caps_;
        call.instance = const_cast<LuaInstance*>(this);
        return call;
    }

    pime_status invoke_value(HostKind kind, uint8_t op, const Value& request,
                             Value* result) {
        std::vector<uint8_t> encoded;
        codec::encode(request, &encoded);
        std::vector<uint8_t> reply;
        pime_status status =
            bridge_->invoke(context(), kind, op, encoded.data(),
                            static_cast<uint32_t>(encoded.size()), &reply);
        *result = Value::null();
        Value envelope;
        if (!reply.empty() &&
            codec::decode(reply.data(), reply.size(), &envelope) == PIME_OK) {
            if (const Value* value = envelope.find("status");
                value && value->is_number())
                status = static_cast<pime_status>(value->as_uint());
            if (const Value* value = envelope.find("result")) *result = *value;
        }
        return status;
    }

    pime_status emit(uint64_t session_id, const uint8_t* data, uint32_t len) {
        return bridge_->emit(session_id, alias_, generation_, data, len);
    }

    lua_State* state() const { return state_; }

    bool protected_call(int args, int results) {
        const int status = lua_pcall(state_, args, results, 0);
        if (status != LUA_OK) {
            const char* message = lua_tostring(state_, -1);
            error_ = message ? message : "lua error";
            lua_pop(state_, 1);
        }
        return status == LUA_OK;
    }

    static LuaInstance* self_of(lua_State* L) {
        lua_getfield(L, LUA_REGISTRYINDEX, kSelf);
        auto* self = static_cast<LuaInstance*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return self;
    }

private:
    static void* allocate(void* ud, void* pointer, size_t old_size,
                          size_t new_size) {
        auto* self = static_cast<LuaInstance*>(ud);
        if (new_size == 0) {
            std::free(pointer);
            self->used_ = self->used_ > old_size ? self->used_ - old_size : 0;
            return nullptr;
        }
        if (self->memory_limit_ &&
            self->used_ - (pointer ? old_size : 0) + new_size >
                self->memory_limit_) {
            return nullptr;
        }
        void* result = std::realloc(pointer, new_size);
        if (result) {
            self->used_ = self->used_ - (pointer ? old_size : 0) + new_size;
        }
        return result;
    }

    static void hook(lua_State* L, lua_Debug*) {
        LuaInstance* self = self_of(L);
        if (!self || self->instruction_budget_ == 0) return;
        if (self->instruction_budget_ <= kHookInstructions) {
            self->instruction_budget_ = 0;
            luaL_error(L, "instruction limit exceeded");
            return;
        }
        self->instruction_budget_ -= kHookInstructions;
    }

    pime_status call_output(const char* name, const uint8_t* in,
                            uint32_t in_len, std::vector<uint8_t>* out) {
        if (!state_) return PIME_CLOSED;
        Value input;
        if (codec::decode(in, in_len, &input) != PIME_OK) return PIME_CORRUPT;
        lua_getglobal(state_, "pime");
        if (!lua_istable(state_, -1)) {
            lua_pop(state_, 1);
            return PIME_CORRUPT;
        }
        lua_getfield(state_, -1, name);
        if (!lua_isfunction(state_, -1)) {
            lua_pop(state_, 2);
            return PIME_UNSUPPORTED;
        }
        lua_remove(state_, -2);
        push_value(state_, input);
        if (!protected_call(1, 1)) return PIME_CORRUPT;
        if (!lua_istable(state_, -1)) {
            lua_pop(state_, 1);
            return PIME_CORRUPT;
        }
        const Value output = to_value(state_, -1);
        lua_pop(state_, 1);
        out->clear();
        codec::encode(output, out);
        return PIME_OK;
    }

    static Value to_value(lua_State* L, int index) {
        index = lua_absindex(L, index);
        switch (lua_type(L, index)) {
            case LUA_TNIL:
            case LUA_TNONE:
                return Value::null();
            case LUA_TBOOLEAN:
                return Value::boolean_v(lua_toboolean(L, index) != 0);
            case LUA_TNUMBER: {
                if (lua_isinteger(L, index)) {
                    const lua_Integer value = lua_tointeger(L, index);
                    if (value < 0) return Value::int_v(value);
                    return Value::uint_v(static_cast<uint64_t>(value));
                }
                return Value::float_v(lua_tonumber(L, index));
            }
            case LUA_TSTRING: {
                size_t length = 0;
                const char* data = lua_tolstring(L, index, &length);
                return Value::text_v(std::string_view(data, length));
            }
            case LUA_TTABLE: {
                const lua_Unsigned length = lua_rawlen(L, index);
                bool array = length > 0;
                lua_pushnil(L);
                while (lua_next(L, index)) {
                    if (lua_type(L, -2) != LUA_TNUMBER) array = false;
                    lua_pop(L, 1);
                }
                if (array) {
                    Value result = Value::array_v();
                    for (lua_Unsigned i = 1; i <= length; ++i) {
                        lua_rawgeti(L, index, static_cast<lua_Integer>(i));
                        result.push(to_value(L, -1));
                        lua_pop(L, 1);
                    }
                    return result;
                }
                Value result = Value::map_v();
                lua_pushnil(L);
                while (lua_next(L, index)) {
                    Value key = to_value(L, -2);
                    if (!key.is_text()) key = Value::text_v(to_string(L, -2));
                    result.set(key.as_text(), to_value(L, -1));
                    lua_pop(L, 1);
                }
                return result;
            }
            default:
                return Value::null();
        }
    }

    static std::string to_string(lua_State* L, int index) {
        switch (lua_type(L, index)) {
            case LUA_TSTRING: {
                size_t length = 0;
                const char* data = lua_tolstring(L, index, &length);
                return std::string(data, length);
            }
            case LUA_TNUMBER: {
                lua_pushvalue(L, index);
                const char* data = lua_tostring(L, -1);
                std::string text = data ? data : "";
                lua_pop(L, 1);
                return text;
            }
            default:
                return std::string();
        }
    }

    static void push_value(lua_State* L, const Value& value) {
        switch (value.type) {
            case codec::Type::Null:
                lua_pushnil(L);
                break;
            case codec::Type::Bool:
                lua_pushboolean(L, value.boolean);
                break;
            case codec::Type::UInt:
                lua_pushinteger(L, static_cast<lua_Integer>(value.uint_value));
                break;
            case codec::Type::Int:
                lua_pushinteger(L, static_cast<lua_Integer>(value.int_value));
                break;
            case codec::Type::Float:
                lua_pushnumber(L, value.float_value);
                break;
            case codec::Type::Text:
            case codec::Type::Bytes:
                lua_pushlstring(L, value.text.data(), value.text.size());
                break;
            case codec::Type::Array: {
                lua_createtable(L, static_cast<int>(value.array.size()), 0);
                int index = 1;
                for (const auto& item : value.array) {
                    push_value(L, item);
                    lua_rawseti(L, -2, index++);
                }
                break;
            }
            case codec::Type::Map: {
                lua_createtable(L, 0, static_cast<int>(value.map.size()));
                for (const auto& entry : value.map) {
                    push_value(L, entry.first);
                    push_value(L, entry.second);
                    lua_rawset(L, -3);
                }
                break;
            }
        }
    }

    /* ---- registration ------------------------------------------------- */

    static void register_api(lua_State* L);
    static int l_workspace_set(lua_State* L);
    static int l_workspace_get(lua_State* L);
    static int l_store_set(lua_State* L);
    static int l_store_get(lua_State* L);
    static int l_store_delete(lua_State* L);
    static int l_store_batch(lua_State* L);
    static int l_store_scan(lua_State* L);
    static int l_store_scan_next(lua_State* L);
    static int l_store_quota(lua_State* L);
    static int l_store_flush(lua_State* L);
    static int l_asset_open(lua_State* L);
    static int l_asset_size(lua_State* L);
    static int l_asset_read(lua_State* L);
    static int l_asset_close(lua_State* L);
    static int l_config_get(lua_State* L);
    static int l_config_list(lua_State* L);
    static int l_config_snapshot(lua_State* L);
    static int l_time(lua_State* L);
    static int l_random_bytes(lua_State* L);
    static int l_timer_start(lua_State* L);
    static int l_timer_cancel(lua_State* L);
    static int l_caps_has(lua_State* L);
    static int l_caps_list(lua_State* L);
    static int l_log(lua_State* L);
    static int l_net_request(lua_State* L);
    static int l_net_result(lua_State* L);
    static int l_net_open(lua_State* L);
    static int l_net_send(lua_State* L);
    static int l_net_close(lua_State* L);
    static int l_crypto(lua_State* L);
    static int l_crypto_unsupported(lua_State* L);
    static int l_audio(lua_State* L);
    static int l_ml_load(lua_State* L);
    static int l_ml_run(lua_State* L);
    static int l_ml_run_async(lua_State* L);
    static int l_ml_cancel(lua_State* L);
    static int l_ml_free(lua_State* L);
    static int l_thread_create(lua_State* L);
    static int l_thread_call(lua_State* L);
    static int l_emit(lua_State* L);
    static int l_ctx(lua_State* L);
    static int l_channel_create(lua_State* L);
    static int l_channel_call(lua_State* L);
    static int l_sync_mutex(lua_State* L);
    static int l_sync_condvar(lua_State* L);
    static int l_sync_method(lua_State* L);
    static int l_shm_create(lua_State* L);
    static int l_shm_method(lua_State* L);

    /* Pushes the reply to a call, or nil + error name. */
    static int push_result(lua_State* L, pime_status status,
                           const Value& result) {
        if (status != PIME_OK) {
            lua_pushnil(L);
            lua_pushstring(L, status_name(status));
            return 2;
        }
        push_value(L, result);
        return 1;
    }

    static int push_bytes_result(lua_State* L, pime_status status,
                                 const Value& result) {
        return push_result(L, status, result);
    }

    static LuaInstance* require(lua_State* L) {
        LuaInstance* self = self_of(L);
        if (!self) luaL_error(L, "pime state missing");
        return self;
    }

    static Value string_field(lua_State* L, int index) {
        return Value::text_v(to_string(L, index));
    }

    static uint64_t handle_arg(lua_State* L, int index) {
        return static_cast<uint64_t>(lua_tointeger(L, index));
    }

    std::string dir_;
    const cap::PluginCapContext* caps_ = nullptr;
    HostBridge* bridge_ = nullptr;
    std::string alias_;
    uint64_t generation_ = 0;
    uint32_t memory_mb_ = 0;
    lua_State* state_ = nullptr;
    size_t used_ = 0;
    size_t memory_limit_ = 0;
    uint64_t instruction_budget_ = 0;
    std::string error_;
    uint64_t session_id_ = 0;
    bool secure_ = false;
    uint32_t host_caps_ = 0;
};

}  // namespace
namespace {

void register_table(lua_State* L, const char* name, const luaL_Reg* entries) {
    lua_newtable(L);
    luaL_setfuncs(L, entries, 0);
    lua_setfield(L, -2, name);
}

}  // namespace

/* ---- workspace / store ----------------------------------------------- */

int LuaInstance::l_workspace_set(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("key", string_field(L, 1));
    request.set("value", to_value(L, 2));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Workspace,
                              static_cast<uint8_t>(WorkspaceOp::Set), request,
                              &result),
        result);
}

int LuaInstance::l_workspace_get(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("key", string_field(L, 1));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Workspace,
                              static_cast<uint8_t>(WorkspaceOp::Get), request,
                              &result),
        result);
}

int LuaInstance::l_store_set(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("key", string_field(L, 1));
    request.set("value", to_value(L, 2));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Store, static_cast<uint8_t>(StoreOp::Set),
                              request, &result),
        result);
}

int LuaInstance::l_store_get(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("key", string_field(L, 1));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Store, static_cast<uint8_t>(StoreOp::Get), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* found = result.find("found");
    if (!found || !found->as_bool()) {
        lua_pushnil(L);
        return 1;
    }
    const Value* value = result.find("value");
    push_value(L, value ? *value : Value::null());
    return 1;
}

int LuaInstance::l_store_delete(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("key", string_field(L, 1));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Store, static_cast<uint8_t>(StoreOp::Del),
                              request, &result),
        result);
}

int LuaInstance::l_store_batch(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("ops", to_value(L, 1));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Store,
                              static_cast<uint8_t>(StoreOp::Batch), request,
                              &result),
        result);
}

int LuaInstance::l_store_scan(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("prefix", string_field(L, 1));
    if (!lua_isnoneornil(L, 2))
        request.set("limit", Value::uint_v(
                                 static_cast<uint64_t>(lua_tointeger(L, 2))));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Store, static_cast<uint8_t>(StoreOp::Scan), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_createtable(L, 0, 2);
    push_value(L, result);
    lua_setfield(L, -2, "data");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "i");
    lua_pushcclosure(L, l_store_scan_next, 1);
    return 1;
}

int LuaInstance::l_store_scan_next(lua_State* L) {
    lua_getfield(L, lua_upvalueindex(1), "data");
    lua_getfield(L, lua_upvalueindex(1), "i");
    const lua_Integer index = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, -1, index);
    if (lua_isnil(L, -1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, "k");
    lua_getfield(L, -2, "v");
    lua_pushinteger(L, index + 1);
    lua_setfield(L, lua_upvalueindex(1), "i");
    return 2;
}

int LuaInstance::l_store_quota(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Store, static_cast<uint8_t>(StoreOp::Quota), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* used = result.find("used");
    const Value* limit = result.find("limit");
    lua_pushinteger(L, static_cast<lua_Integer>(used ? used->as_uint() : 0));
    lua_pushinteger(L, static_cast<lua_Integer>(limit ? limit->as_uint() : 0));
    return 2;
}

int LuaInstance::l_store_flush(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Store, static_cast<uint8_t>(StoreOp::Flush), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- asset ------------------------------------------------------------ */

int LuaInstance::l_asset_open(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("path", string_field(L, 1));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Asset, static_cast<uint8_t>(AssetOp::Open), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* handle = result.find("handle");
    lua_pushinteger(L, static_cast<lua_Integer>(handle ? handle->as_uint() : 0));
    return 1;
}

int LuaInstance::l_asset_size(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("handle", Value::uint_v(handle_arg(L, 1)));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Asset, static_cast<uint8_t>(AssetOp::Size), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* size = result.find("size");
    lua_pushinteger(L, static_cast<lua_Integer>(size ? size->as_uint() : 0));
    return 1;
}

int LuaInstance::l_asset_read(lua_State* L) {
    LuaInstance* self = require(L);
    uint8_t op = static_cast<uint8_t>(AssetOp::Read);
    if (lua_isinteger(L, lua_upvalueindex(1)))
        op = static_cast<uint8_t>(lua_tointeger(L, lua_upvalueindex(1)));
    Value request = Value::map_v();
    request.set("handle", Value::uint_v(handle_arg(L, 1)));
    request.set("offset", Value::uint_v(
                             static_cast<uint64_t>(lua_tointeger(L, 2))));
    request.set("len", Value::uint_v(
                           static_cast<uint64_t>(lua_tointeger(L, 3))));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Asset, op, request, &result), result);
}

int LuaInstance::l_asset_close(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("handle", Value::uint_v(handle_arg(L, 1)));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Asset, static_cast<uint8_t>(AssetOp::Close), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- config ----------------------------------------------------------- */

int LuaInstance::l_config_get(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("key", string_field(L, 1));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Config, static_cast<uint8_t>(ConfigOp::Get), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* found = result.find("found");
    if (!found || !found->as_bool()) {
        lua_pushnil(L);
        return 1;
    }
    const Value* value = result.find("value");
    push_value(L, value ? *value : Value::null());
    return 1;
}

int LuaInstance::l_config_list(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("prefix", string_field(L, 1));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Config,
                              static_cast<uint8_t>(ConfigOp::List), request,
                              &result),
        result);
}

int LuaInstance::l_config_snapshot(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Config, static_cast<uint8_t>(ConfigOp::Snapshot), request,
        &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* id = result.find("id");
    lua_pushinteger(L, static_cast<lua_Integer>(id ? id->as_uint() : 0));
    return 1;
}

/* ---- time / random / timer / caps ------------------------------------ */

int LuaInstance::l_time(lua_State* L) {
    LuaInstance* self = require(L);
    const uint32_t kind =
        static_cast<uint32_t>(lua_tointeger(L, lua_upvalueindex(1)));
    Value request = Value::map_v();
    request.set("kind", Value::uint_v(kind));
    Value result;
    const pime_status status =
        self->invoke_value(HostKind::Time, 0, request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* value = result.find("value");
    lua_pushinteger(L, static_cast<lua_Integer>(value ? value->as_uint() : 0));
    return 1;
}

int LuaInstance::l_random_bytes(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("len", Value::uint_v(
                           static_cast<uint64_t>(lua_tointeger(L, 1))));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Random, 0, request, &result), result);
}

int LuaInstance::l_timer_start(lua_State* L) {
    LuaInstance* self = require(L);
    const bool repeat = lua_toboolean(L, lua_upvalueindex(1)) != 0;
    Value request = Value::map_v();
    request.set(repeat ? "interval_ms" : "delay_ms",
                Value::uint_v(static_cast<uint64_t>(lua_tointeger(L, 1))));
    request.set("event_id", string_field(L, 2));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Timer,
        static_cast<uint8_t>(repeat ? TimerOp::Every : TimerOp::After),
        request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* id = result.find("id");
    lua_pushinteger(L, static_cast<lua_Integer>(id ? id->as_uint() : 0));
    return 1;
}

int LuaInstance::l_timer_cancel(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("id", Value::uint_v(handle_arg(L, 1)));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Timer, static_cast<uint8_t>(TimerOp::Cancel), request,
        &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaInstance::l_caps_has(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Caps, static_cast<uint8_t>(CapsOp::Query), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const std::string wanted = to_string(L, 1);
    bool found = false;
    if (const Value* list = result.find("capabilities");
        list && list->is_array()) {
        for (const auto& item : list->array)
            if (std::string(item.as_text()) == wanted) found = true;
    }
    lua_pushboolean(L, found);
    return 1;
}

int LuaInstance::l_caps_list(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Caps, static_cast<uint8_t>(CapsOp::Query), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* list = result.find("capabilities");
    push_value(L, list ? *list : Value::array_v());
    return 1;
}

/* ---- log -------------------------------------------------------------- */

int LuaInstance::l_log(lua_State* L) {
    LuaInstance* self = require(L);
    int level = PIME_LOG_INFO;
    int tag_index = 1;
    int msg_index = 2;
    if (lua_toboolean(L, lua_upvalueindex(2))) {
        level = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    } else if (lua_type(L, 1) == LUA_TNUMBER) {
        level = static_cast<int>(lua_tointeger(L, 1));
        tag_index = 2;
        msg_index = 3;
    } else {
        const std::string name = to_string(L, 1);
        if (name == "error") level = PIME_LOG_ERROR;
        else if (name == "warn") level = PIME_LOG_WARN;
        else if (name == "debug") level = PIME_LOG_DEBUG;
        else if (name == "trace") level = PIME_LOG_TRACE;
        else level = PIME_LOG_INFO;
        tag_index = 2;
        msg_index = 3;
    }
    Value request = Value::map_v();
    request.set("level", Value::uint_v(static_cast<uint64_t>(level)));
    request.set("tag", string_field(L, tag_index));
    request.set("message", string_field(L, msg_index));
    Value result;
    const pime_status status =
        self->invoke_value(HostKind::Log, 0, request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    return 0;
}

/* ---- net -------------------------------------------------------------- */

int LuaInstance::l_net_request(lua_State* L) {
    LuaInstance* self = require(L);
    const Value request = to_value(L, 1);
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Net, static_cast<uint8_t>(NetOp::Http), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* id = result.find("request");
    lua_pushinteger(L, static_cast<lua_Integer>(id ? id->as_uint() : 0));
    return 1;
}

int LuaInstance::l_net_result(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("request", Value::uint_v(handle_arg(L, 1)));
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Net,
                              static_cast<uint8_t>(NetOp::Result), request,
                              &result),
        result);
}

int LuaInstance::l_net_open(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("proto", string_field(L, 1));
    request.set("url", string_field(L, 2));
    if (!lua_isnoneornil(L, 3)) request.set("options", to_value(L, 3));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Net, static_cast<uint8_t>(NetOp::Open), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* id = result.find("id");
    lua_pushinteger(L, static_cast<lua_Integer>(id ? id->as_uint() : 0));
    return 1;
}

int LuaInstance::l_net_send(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("id", Value::uint_v(handle_arg(L, 1)));
    request.set("data", to_value(L, 2));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Net, static_cast<uint8_t>(NetOp::Send), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaInstance::l_net_close(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("id", Value::uint_v(handle_arg(L, 1)));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Net, static_cast<uint8_t>(NetOp::Close), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- crypto ----------------------------------------------------------- */

int LuaInstance::l_crypto(lua_State* L) {
    LuaInstance* self = require(L);
    const std::string name = lua_tostring(L, lua_upvalueindex(1));
    Value request = Value::map_v();
    uint8_t op = 0;
    if (name == "digest") {
        request.set("alg", string_field(L, 1));
        request.set("data", to_value(L, 2));
    } else if (name == "mac") {
        op = 1;
        request.set("alg", string_field(L, 1));
        request.set("key", to_value(L, 2));
        request.set("data", to_value(L, 3));
    } else if (name == "cipher" || name == "encrypt") {
        op = 2;
        request.set("alg", string_field(L, 1));
        request.set("key", to_value(L, 2));
        request.set("iv", to_value(L, 3));
        request.set("aad", to_value(L, 4));
        request.set("plaintext", to_value(L, 5));
    } else if (name == "decrypt") {
        op = 3;
        request.set("alg", string_field(L, 1));
        request.set("key", to_value(L, 2));
        request.set("iv", to_value(L, 3));
        request.set("aad", to_value(L, 4));
        request.set("ciphertext", to_value(L, 5));
        request.set("tag", to_value(L, 6));
    } else if (name == "random") {
        op = 4;
        request.set("len", Value::uint_v(
                               static_cast<uint64_t>(lua_tointeger(L, 1))));
    } else if (name == "verify") {
        op = 5;
        request.set("key", to_value(L, 1));
        request.set("alg", string_field(L, 2));
        request.set("data", to_value(L, 3));
        request.set("sig", to_value(L, 4));
    } else if (name == "sign") {
        op = 6;
        request.set("key", to_value(L, 1));
        request.set("alg", string_field(L, 2));
        request.set("data", to_value(L, 3));
    } else if (name == "kdf") {
        op = 7;
        request.set("alg", string_field(L, 1));
        request.set("key", to_value(L, 2));
        request.set("salt", to_value(L, 3));
        request.set("info", to_value(L, 4));
        request.set("len", Value::uint_v(
                               static_cast<uint64_t>(lua_tointeger(L, 5))));
    } else if (name == "x509_verify") {
        op = 8;
        request.set("cert", to_value(L, 1));
        request.set("ca", to_value(L, 2));
    } else {
        lua_pushnil(L);
        lua_pushstring(L, status_name(PIME_UNSUPPORTED));
        return 2;
    }
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Crypto, op, request, &result), result);
}

int LuaInstance::l_crypto_unsupported(lua_State* L) {
    (void)L;
    lua_pushnil(L);
    lua_pushstring(L, status_name(PIME_UNSUPPORTED));
    return 2;
}

/* ---- audio ------------------------------------------------------------ */

int LuaInstance::l_audio(lua_State* L) {
    LuaInstance* self = require(L);
    const int op = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    Value request = Value::map_v();
    if (op == 6) {
        Value frame = to_value(L, 1);
        if (frame.is_map()) request = frame;
        const Value* format = request.find("format");
        if (format) {
            if (const Value* rate = format->find("rate"))
                request.set("from_rate", *rate);
            if (const Value* channels = format->find("channels"))
                request.set("from_channels", *channels);
            if (const Value* value = format->find("format"))
                request.set("from_format", *value);
        }
        const Value to = to_value(L, 2);
        if (const Value* rate = to.find("rate")) request.set("to_rate", *rate);
        if (const Value* channels = to.find("channels"))
            request.set("to_channels", *channels);
        if (const Value* value = to.find("format"))
            request.set("to_format", *value);
    } else {
        const Value first = to_value(L, 1);
        if (first.is_map()) request = first;
        if (!lua_isnoneornil(L, 2)) {
            const Value options = to_value(L, 2);
            if (options.is_map())
                for (const auto& entry : options.map)
                    request.set(entry.first.as_text(), entry.second);
        }
    }
    Value result;
    return push_result(
        L, self->invoke_value(HostKind::Audio, static_cast<uint8_t>(op),
                              request, &result),
        result);
}

/* ---- ml --------------------------------------------------------------- */

int LuaInstance::l_ml_load(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("model", string_field(L, 1));
    if (!lua_isnoneornil(L, 2)) request.set("options", to_value(L, 2));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Ml, static_cast<uint8_t>(MlOp::Load), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* session = result.find("session");
    lua_pushinteger(L,
                    static_cast<lua_Integer>(session ? session->as_uint() : 0));
    return 1;
}

int LuaInstance::l_ml_run(lua_State* L) {
    LuaInstance* self = require(L);
    const Value tensor = to_value(L, 2);
    std::vector<uint8_t> encoded;
    codec::encode(tensor, &encoded);
    Value request = Value::map_v();
    request.set("session", Value::uint_v(handle_arg(L, 1)));
    request.set("in", Value::bytes(std::string_view(
                           reinterpret_cast<const char*>(encoded.data()),
                           encoded.size())));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Ml, static_cast<uint8_t>(MlOp::Run), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    if (result.is_text() || result.is_bytes()) {
        Value decoded;
        const std::string_view bytes = result.as_bytes();
        if (codec::decode(
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                &decoded) == PIME_OK) {
            push_value(L, decoded);
            return 1;
        }
    }
    push_value(L, result);
    return 1;
}

int LuaInstance::l_ml_run_async(lua_State* L) {
    LuaInstance* self = require(L);
    const Value tensor = to_value(L, 2);
    std::vector<uint8_t> encoded;
    codec::encode(tensor, &encoded);
    Value request = Value::map_v();
    request.set("session", Value::uint_v(handle_arg(L, 1)));
    request.set("in", Value::bytes(std::string_view(
                           reinterpret_cast<const char*>(encoded.data()),
                           encoded.size())));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Ml, static_cast<uint8_t>(MlOp::RunAsync), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* job = result.find("job");
    lua_pushinteger(L, static_cast<lua_Integer>(job ? job->as_uint() : 0));
    return 1;
}

int LuaInstance::l_ml_cancel(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("job", Value::uint_v(handle_arg(L, 1)));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Ml, static_cast<uint8_t>(MlOp::Cancel), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaInstance::l_ml_free(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("session", Value::uint_v(handle_arg(L, 1)));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Ml, static_cast<uint8_t>(MlOp::Free), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- threads / emit --------------------------------------------------- */

int LuaInstance::l_thread_create(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = to_value(L, 1);
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Thread, static_cast<uint8_t>(ThreadOp::Create), request,
        &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* tid = result.find("tid");
    lua_pushinteger(L, static_cast<lua_Integer>(tid ? tid->as_uint() : 0));
    return 1;
}

int LuaInstance::l_thread_call(lua_State* L) {
    LuaInstance* self = require(L);
    const int op = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(handle_arg(L, 1)));
    if (op == static_cast<int>(ThreadOp::Post)) {
        const Value message = to_value(L, 2);
        std::vector<uint8_t> encoded;
        codec::encode(message, &encoded);
        request.set("data", Value::bytes(std::string_view(
                                reinterpret_cast<const char*>(encoded.data()),
                                encoded.size())));
        if (!lua_isnoneornil(L, 3))
            request.set("timeout_ms",
                        Value::uint_v(static_cast<uint64_t>(
                            lua_tointeger(L, 3))));
    } else if (op == static_cast<int>(ThreadOp::Recv)) {
        request.set("timeout_ms", Value::uint_v(static_cast<uint64_t>(
                                      lua_isnoneornil(L, 2)
                                          ? 0xFFFFFFFFu
                                          : lua_tointeger(L, 2))));
    } else if (op == static_cast<int>(ThreadOp::Park) ||
               op == static_cast<int>(ThreadOp::Join)) {
        if (!lua_isnoneornil(L, 2))
            request.set("timeout_ms",
                        Value::uint_v(static_cast<uint64_t>(
                            lua_tointeger(L, 2))));
    }
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Thread, static_cast<uint8_t>(op), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    if (op == static_cast<int>(ThreadOp::Recv) &&
        (result.is_text() || result.is_bytes())) {
        Value decoded;
        const std::string_view bytes = result.as_bytes();
        if (codec::decode(
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                &decoded) == PIME_OK) {
            push_value(L, decoded);
            return 1;
        }
        push_value(L, result);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaInstance::l_emit(lua_State* L) {
    LuaInstance* self = require(L);
    const uint64_t session = static_cast<uint64_t>(lua_tointeger(L, 1));
    const Value output = to_value(L, 2);
    std::vector<uint8_t> encoded;
    codec::encode(output, &encoded);
    const pime_status status = self->emit(
        session, encoded.data(), static_cast<uint32_t>(encoded.size()));
    if (status != PIME_OK) return push_result(L, status, Value::null());
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- ctx / channel / sync / shm -------------------------------------- */

int LuaInstance::l_ctx(lua_State* L) {
    LuaInstance* self = require(L);
    const int op = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    Value request = Value::map_v();
    if (op == static_cast<int>(CtxOp::Acquire)) {
        request.set("name", string_field(L, 1));
        request.set("kind", string_field(L, 2));
        uint64_t flags = 0;
        if (!lua_isnoneornil(L, 4) && lua_istable(L, 4)) {
            lua_getfield(L, 4, "create");
            if (lua_toboolean(L, -1)) flags |= PIME_CTX_CREATE;
            lua_pop(L, 1);
            lua_getfield(L, 4, "pin");
            if (lua_toboolean(L, -1)) flags |= PIME_CTX_PIN;
            lua_pop(L, 1);
            lua_getfield(L, 4, "share");
            if (lua_toboolean(L, -1)) flags |= PIME_CTX_SHARE;
            lua_pop(L, 1);
            lua_getfield(L, 4, "schema");
            if (lua_isinteger(L, -1))
                request.set("schema",
                            Value::uint_v(static_cast<uint64_t>(
                                lua_tointeger(L, -1))));
            lua_pop(L, 1);
        }
        request.set("flags", Value::uint_v(flags));
    } else if (op == static_cast<int>(CtxOp::Drop) ||
               op == static_cast<int>(CtxOp::Info)) {
        request.set("name", string_field(L, 1));
        request.set("kind", string_field(L, 2));
    } else if (op == static_cast<int>(CtxOp::Pin)) {
        request.set("handle", Value::uint_v(handle_arg(L, 1)));
        request.set("pin", Value::boolean_v(lua_toboolean(L, 2) != 0));
    } else if (op == static_cast<int>(CtxOp::Call)) {
        request.set("handle", Value::uint_v(handle_arg(L, 1)));
        const Value input = to_value(L, 3);
        std::vector<uint8_t> encoded;
        codec::encode(input, &encoded);
        request.set("in", Value::bytes(std::string_view(
                               reinterpret_cast<const char*>(encoded.data()),
                               encoded.size())));
    } else {
        request.set("handle", Value::uint_v(handle_arg(L, 1)));
    }
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Ctx, static_cast<uint8_t>(op), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    if (op == static_cast<int>(CtxOp::Acquire)) {
        const Value* handle = result.find("handle");
        lua_pushinteger(L,
                        static_cast<lua_Integer>(handle ? handle->as_uint() : 0));
        return 1;
    }
    if (op == static_cast<int>(CtxOp::Call) && result.is_bytes()) {
        Value decoded;
        const std::string_view bytes = result.as_bytes();
        if (codec::decode(
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                &decoded) == PIME_OK) {
            push_value(L, decoded);
            return 1;
        }
    }
    push_value(L, result);
    return 1;
}

int LuaInstance::l_channel_create(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("capacity",
                Value::uint_v(static_cast<uint64_t>(
                    lua_isnoneornil(L, 1) ? 0 : lua_tointeger(L, 1))));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Channel, static_cast<uint8_t>(ChannelOp::Create), request,
        &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* handle = result.find("handle");
    lua_pushinteger(L, static_cast<lua_Integer>(handle ? handle->as_uint() : 0));
    return 1;
}

int LuaInstance::l_channel_call(lua_State* L) {
    LuaInstance* self = require(L);
    const int op = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    Value request = Value::map_v();
    request.set("handle", Value::uint_v(handle_arg(L, 1)));
    if (op == static_cast<int>(ChannelOp::Send)) {
        Value message = to_value(L, 2);
        std::vector<uint8_t> encoded;
        codec::encode(message, &encoded);
        request.set("data", Value::bytes(std::string_view(
                                reinterpret_cast<const char*>(encoded.data()),
                                encoded.size())));
        if (!lua_isnoneornil(L, 3))
            request.set("timeout_ms",
                        Value::uint_v(static_cast<uint64_t>(
                            lua_tointeger(L, 3))));
    } else if (op == static_cast<int>(ChannelOp::Recv)) {
        if (!lua_isnoneornil(L, 2))
            request.set("timeout_ms",
                        Value::uint_v(static_cast<uint64_t>(
                            lua_tointeger(L, 2))));
    } else if (op == static_cast<int>(ChannelOp::Close)) {
        const std::string mode = to_string(L, 2);
        request.set("abort", Value::boolean_v(mode == "abort"));
    }
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Channel, static_cast<uint8_t>(op), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    if (op == static_cast<int>(ChannelOp::Recv) && result.is_map()) {
        push_value(L, result);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaInstance::l_sync_method(lua_State* L) {
    LuaInstance* self = require(L);
    const uint64_t handle =
        static_cast<uint64_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const int op = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
    Value request = Value::map_v();
    request.set("handle", Value::uint_v(handle));
    const int base = lua_istable(L, 1) ? 2 : 1;
    if (op == static_cast<int>(SyncOp::MutexLock)) {
        if (!lua_isnoneornil(L, base))
            request.set("timeout_ms", Value::uint_v(static_cast<uint64_t>(
                                          lua_tointeger(L, base))));
    } else if (op == static_cast<int>(SyncOp::CondvarWait)) {
        if (lua_istable(L, base)) {
            lua_getfield(L, base, "handle");
            request.set("mutex", Value::uint_v(static_cast<uint64_t>(
                                     lua_tointeger(L, -1))));
            lua_pop(L, 1);
        }
        if (!lua_isnoneornil(L, base + 1))
            request.set("timeout_ms", Value::uint_v(static_cast<uint64_t>(
                                          lua_tointeger(L, base + 1))));
    }
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Sync, static_cast<uint8_t>(op), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaInstance::l_sync_mutex(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    if (!lua_isnoneornil(L, 1) && lua_istable(L, 1)) {
        lua_getfield(L, 1, "recursive");
        request.set("recursive", Value::boolean_v(lua_toboolean(L, -1) != 0));
        lua_pop(L, 1);
    }
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Sync, static_cast<uint8_t>(SyncOp::MutexCreate), request,
        &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* value = result.find("handle");
    const lua_Integer handle =
        static_cast<lua_Integer>(value ? value->as_uint() : 0);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, handle);
    lua_setfield(L, -2, "handle");
    lua_pushinteger(L, handle);
    lua_pushinteger(L, static_cast<lua_Integer>(SyncOp::MutexLock));
    lua_pushcclosure(L, l_sync_method, 2);
    lua_setfield(L, -2, "lock");
    lua_pushinteger(L, handle);
    lua_pushinteger(L, static_cast<lua_Integer>(SyncOp::MutexUnlock));
    lua_pushcclosure(L, l_sync_method, 2);
    lua_setfield(L, -2, "unlock");
    return 1;
}

int LuaInstance::l_sync_condvar(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Sync, static_cast<uint8_t>(SyncOp::CondvarCreate), request,
        &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* value = result.find("handle");
    const lua_Integer handle =
        static_cast<lua_Integer>(value ? value->as_uint() : 0);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, handle);
    lua_setfield(L, -2, "handle");
    const struct {
        const char* name;
        SyncOp op;
    } methods[] = {{"wait", SyncOp::CondvarWait},
                   {"signal", SyncOp::CondvarSignal},
                   {"broadcast", SyncOp::CondvarBroadcast}};
    for (const auto& method : methods) {
        lua_pushinteger(L, handle);
        lua_pushinteger(L, static_cast<lua_Integer>(method.op));
        lua_pushcclosure(L, l_sync_method, 2);
        lua_setfield(L, -2, method.name);
    }
    return 1;
}

int LuaInstance::l_shm_create(lua_State* L) {
    LuaInstance* self = require(L);
    Value request = Value::map_v();
    request.set("size",
                Value::uint_v(static_cast<uint64_t>(lua_tointeger(L, 1))));
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Shm, static_cast<uint8_t>(ShmOp::Create), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    const Value* value = result.find("handle");
    const lua_Integer handle =
        static_cast<lua_Integer>(value ? value->as_uint() : 0);
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, handle);
    lua_setfield(L, -2, "handle");
    const struct {
        const char* name;
        ShmOp op;
    } methods[] = {{"read", ShmOp::Read},     {"write", ShmOp::Write},
                   {"resize", ShmOp::Resize}, {"map", ShmOp::Map},
                   {"unmap", ShmOp::Unmap},   {"close", ShmOp::Close}};
    for (const auto& method : methods) {
        lua_pushinteger(L, handle);
        lua_pushinteger(L, static_cast<lua_Integer>(method.op));
        lua_pushcclosure(L, l_shm_method, 2);
        lua_setfield(L, -2, method.name);
    }
    return 1;
}

int LuaInstance::l_shm_method(lua_State* L) {
    LuaInstance* self = require(L);
    const uint64_t handle =
        static_cast<uint64_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const int op = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
    Value request = Value::map_v();
    request.set("handle", Value::uint_v(handle));
    const int base = lua_istable(L, 1) ? 2 : 1;
    if (op == static_cast<int>(ShmOp::Read)) {
        request.set("offset", Value::uint_v(
                                  static_cast<uint64_t>(lua_tointeger(L, base))));
        request.set("len", Value::uint_v(static_cast<uint64_t>(
                               lua_tointeger(L, base + 1))));
    } else if (op == static_cast<int>(ShmOp::Write)) {
        request.set("offset", Value::uint_v(
                                  static_cast<uint64_t>(lua_tointeger(L, base))));
        request.set("data", to_value(L, base + 1));
    } else if (op == static_cast<int>(ShmOp::Resize)) {
        request.set("size", Value::uint_v(
                                static_cast<uint64_t>(lua_tointeger(L, base))));
    }
    Value result;
    const pime_status status = self->invoke_value(
        HostKind::Shm, static_cast<uint8_t>(op), request, &result);
    if (status != PIME_OK) return push_result(L, status, result);
    if (op == static_cast<int>(ShmOp::Read) ||
        op == static_cast<int>(ShmOp::Map)) {
        push_value(L, result);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- registration ----------------------------------------------------- */

void LuaInstance::register_api(lua_State* L) {
    lua_newtable(L);

    const luaL_Reg workspace[] = {
        {"set", l_workspace_set}, {"get", l_workspace_get}, {nullptr, nullptr}};
    register_table(L, "workspace", workspace);

    const luaL_Reg store[] = {
        {"set", l_store_set},       {"get", l_store_get},
        {"delete", l_store_delete}, {"batch", l_store_batch},
        {"scan", l_store_scan},     {"quota", l_store_quota},
        {"flush", l_store_flush},   {nullptr, nullptr}};
    register_table(L, "store", store);

    const luaL_Reg asset[] = {
        {"open", l_asset_open}, {"size", l_asset_size}, {"read", l_asset_read},
        {"close", l_asset_close}, {nullptr, nullptr}};
    register_table(L, "asset", asset);
    lua_getfield(L, -1, "asset");
    lua_pushinteger(L, static_cast<lua_Integer>(AssetOp::Map));
    lua_pushcclosure(L, l_asset_read, 1);
    lua_setfield(L, -2, "map");
    lua_pop(L, 1);

    const luaL_Reg config[] = {
        {"get", l_config_get},           {"list", l_config_list},
        {"snapshot", l_config_snapshot}, {nullptr, nullptr}};
    register_table(L, "config", config);

    lua_newtable(L);
    const char* time_names[] = {"monotonic_ms", "wall_ms", "timezone_offset"};
    for (int i = 0; i < 3; ++i) {
        lua_pushinteger(L, i);
        lua_pushcclosure(L, l_time, 1);
        lua_setfield(L, -2, time_names[i]);
    }
    lua_setfield(L, -2, "time");

    lua_newtable(L);
    lua_pushcfunction(L, l_random_bytes);
    lua_setfield(L, -2, "bytes");
    lua_setfield(L, -2, "random");

    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, l_timer_start, 1);
    lua_setfield(L, -2, "after");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, l_timer_start, 1);
    lua_setfield(L, -2, "every");
    lua_pushcfunction(L, l_timer_cancel);
    lua_setfield(L, -2, "cancel");
    lua_setfield(L, -2, "timer");

    lua_newtable(L);
    lua_pushcfunction(L, l_caps_has);
    lua_setfield(L, -2, "has");
    lua_pushcfunction(L, l_caps_list);
    lua_setfield(L, -2, "list");
    lua_setfield(L, -2, "caps");

    lua_newtable(L);
    const struct {
        const char* name;
        int level;
    } levels[] = {{"error", PIME_LOG_ERROR}, {"warn", PIME_LOG_WARN},
                  {"info", PIME_LOG_INFO},   {"debug", PIME_LOG_DEBUG}};
    for (const auto& level : levels) {
        lua_pushinteger(L, level.level);
        lua_pushboolean(L, 1);
        lua_pushcclosure(L, l_log, 2);
        lua_setfield(L, -2, level.name);
    }
    lua_newtable(L);
    lua_pushcfunction(L, l_log);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "log");

    lua_newtable(L);
    lua_pushcfunction(L, l_net_request);
    lua_setfield(L, -2, "request");
    lua_pushcfunction(L, l_net_result);
    lua_setfield(L, -2, "result");
    lua_pushcfunction(L, l_net_open);
    lua_setfield(L, -2, "open");
    lua_pushcfunction(L, l_net_send);
    lua_setfield(L, -2, "send");
    lua_pushcfunction(L, l_net_close);
    lua_setfield(L, -2, "close");
    lua_setfield(L, -2, "net");

    lua_newtable(L);
    const char* crypto_names[] = {"digest", "mac",     "cipher", "decrypt",
                                  "random", "verify",  "sign",   "kdf",
                                  "x509_verify"};
    for (const char* name : crypto_names) {
        lua_pushstring(L, name);
        lua_pushcclosure(L, l_crypto, 1);
        lua_setfield(L, -2, name);
    }
    for (const char* name : {"key_import", "key_destroy"}) {
        lua_pushcfunction(L, l_crypto_unsupported);
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "crypto");

    lua_newtable(L);
    const struct {
        const char* name;
        AudioOp op;
    } audio_functions[] = {
        {"capture_start", AudioOp::CaptureStart},
        {"capture_stop", AudioOp::CaptureStop},
        {"capture_read", AudioOp::CaptureRead},
        {"play", AudioOp::PlaybackStart},
        {"playback_start", AudioOp::PlaybackStart},
        {"playback_stop", AudioOp::PlaybackStop},
        {"playback_write", AudioOp::PlaybackWrite}};
    for (const auto& entry : audio_functions) {
        lua_pushinteger(L, static_cast<lua_Integer>(entry.op));
        lua_pushcclosure(L, l_audio, 1);
        lua_setfield(L, -2, entry.name);
    }
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(AudioOp::PcmConvert));
    lua_pushcclosure(L, l_audio, 1);
    lua_setfield(L, -2, "convert");
    lua_setfield(L, -2, "pcm");
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(AudioOp::OpusEncode));
    lua_pushcclosure(L, l_audio, 1);
    lua_setfield(L, -2, "encode");
    lua_pushinteger(L, static_cast<lua_Integer>(AudioOp::OpusDecode));
    lua_pushcclosure(L, l_audio, 1);
    lua_setfield(L, -2, "decode");
    lua_setfield(L, -2, "opus");
    lua_setfield(L, -2, "audio");

    lua_newtable(L);
    lua_pushcfunction(L, l_ml_load);
    lua_setfield(L, -2, "load");
    lua_pushcfunction(L, l_ml_run);
    lua_setfield(L, -2, "run");
    lua_pushcfunction(L, l_ml_run_async);
    lua_setfield(L, -2, "run_async");
    lua_pushcfunction(L, l_ml_cancel);
    lua_setfield(L, -2, "cancel");
    lua_pushcfunction(L, l_ml_free);
    lua_setfield(L, -2, "free");
    lua_setfield(L, -2, "ml");

    lua_newtable(L);
    lua_pushcfunction(L, l_thread_create);
    lua_setfield(L, -2, "create");
    const struct {
        const char* name;
        ThreadOp op;
    } thread_functions[] = {{"post", ThreadOp::Post},
                            {"recv", ThreadOp::Recv},
                            {"park", ThreadOp::Park},
                            {"unpark", ThreadOp::Unpark},
                            {"close", ThreadOp::Close},
                            {"join", ThreadOp::Join},
                            {"info", ThreadOp::Info}};
    for (const auto& entry : thread_functions) {
        lua_pushinteger(L, static_cast<lua_Integer>(entry.op));
        lua_pushcclosure(L, l_thread_call, 1);
        lua_setfield(L, -2, entry.name);
    }
    lua_setfield(L, -2, "thread");
    lua_pushcfunction(L, l_emit);
    lua_setfield(L, -2, "emit");

    lua_newtable(L);
    const struct {
        const char* name;
        CtxOp op;
    } ctx_functions[] = {{"acquire", CtxOp::Acquire},
                         {"release", CtxOp::Release},
                         {"pin", CtxOp::Pin},
                         {"drop", CtxOp::Drop},
                         {"info", CtxOp::Info},
                         {"call", CtxOp::Call}};
    for (const auto& entry : ctx_functions) {
        lua_pushinteger(L, static_cast<lua_Integer>(entry.op));
        lua_pushcclosure(L, l_ctx, 1);
        lua_setfield(L, -2, entry.name);
    }
    lua_setfield(L, -2, "ctx");

    lua_newtable(L);
    lua_pushcfunction(L, l_channel_create);
    lua_setfield(L, -2, "create");
    const struct {
        const char* name;
        ChannelOp op;
    } channel_functions[] = {{"send", ChannelOp::Send},
                             {"recv", ChannelOp::Recv},
                             {"close", ChannelOp::Close}};
    for (const auto& entry : channel_functions) {
        lua_pushinteger(L, static_cast<lua_Integer>(entry.op));
        lua_pushcclosure(L, l_channel_call, 1);
        lua_setfield(L, -2, entry.name);
    }
    lua_setfield(L, -2, "channel");

    lua_newtable(L);
    lua_pushcfunction(L, l_sync_mutex);
    lua_setfield(L, -2, "mutex");
    lua_pushcfunction(L, l_sync_condvar);
    lua_setfield(L, -2, "condvar");
    lua_setfield(L, -2, "sync");

    lua_newtable(L);
    lua_pushcfunction(L, l_shm_create);
    lua_setfield(L, -2, "create");
    lua_setfield(L, -2, "shm");

    lua_setglobal(L, "pime");
}

pime_status create_lua_instance(const codec::Manifest& manifest,
                                const std::string& package_dir,
                                const cap::PluginCapContext* caps,
                                HostBridge* bridge, const std::string& alias,
                                uint64_t generation,
                                std::unique_ptr<PluginInstance>* out) {
    if (!out || !caps || !bridge) return PIME_INVALID;
    *out = std::make_unique<LuaInstance>(
        package_dir, caps, bridge, alias, generation,
        manifest.limits.memory_mb, manifest.limits.instructions);
    return PIME_OK;
}

}  // namespace pime::host
