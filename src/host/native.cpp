#include "plugin_instance.hpp"

#include <pime/native_plugin.h>

#include <cstddef>

#include "capabilities/cap_context.hpp"
#include "platform/platform.hpp"

#if defined(PIME_BUILD_WASM)
#include "host/wasm/wasm_instance.hpp"
#endif

#if defined(PIME_BUILD_LUA)
#include "host/lua/lua_instance.hpp"
#endif

namespace pime::host {

namespace {

class NativeInstance;

struct NativeHostAdapter {
    pime_native_host api{};
    NativeInstance* owner = nullptr;
};

class NativeInstance : public PluginInstance {
public:
    NativeInstance(platform::Library library, pime_native_plugin vtable,
                   const cap::PluginCapContext* caps, HostBridge* bridge,
                   std::string alias, uint64_t generation, uint32_t max_frame)
        : library_(std::move(library)),
          vtable_(vtable),
          caps_(caps),
          bridge_(bridge),
          alias_(std::move(alias)),
          generation_(generation),
          max_frame_(max_frame) {}

    ~NativeInstance() override { shutdown(); }

    pime_status init(const std::vector<uint8_t>& config) override {
        if (!vtable_.init) return PIME_CORRUPT;
        void* state = nullptr;
        pime_status st =
            vtable_.init(config.data(), static_cast<uint32_t>(config.size()), &state);
        if (st != PIME_OK) return st;
        state_ = state;
        if (has_set_host() && vtable_.set_host) {
            host_ = std::make_unique<NativeHostAdapter>();
            host_->owner = this;
            host_->api.size = sizeof(pime_native_host);
            host_->api.version = PIME_ABI_MAJOR;
            host_->api.ud = host_.get();
            host_->api.check = &NativeInstance::host_check;
            host_->api.invoke = &NativeInstance::host_invoke;
            host_->api.emit = &NativeInstance::host_emit;
            st = vtable_.set_host(state_, &host_->api);
            if (st != PIME_OK) {
                shutdown();
                return st;
            }
        }
        return PIME_OK;
    }

    pime_status process(const uint8_t* in, uint32_t in_len,
                        std::vector<uint8_t>* out) override {
        if (!state_ || !vtable_.process) return PIME_CLOSED;
        return call(vtable_.process, in, in_len, out);
    }

    pime_status on_event(const uint8_t* evt, uint32_t evt_len,
                         std::vector<uint8_t>* out) override {
        if (!state_ || !vtable_.on_event) return PIME_UNSUPPORTED;
        return call(vtable_.on_event, evt, evt_len, out);
    }

    void set_session(uint64_t session_id, bool secure,
                     uint32_t host_caps) override {
        session_id_ = session_id;
        secure_ = secure;
        host_caps_ = host_caps;
    }

    void shutdown() override {
        if (state_ && vtable_.shutdown) vtable_.shutdown(state_);
        state_ = nullptr;
        host_.reset();
    }

    bool alive() const override { return state_ != nullptr; }

private:
    using Call = pime_status (*)(void*, const uint8_t*, uint32_t, uint8_t*,
                                 uint32_t, uint32_t*);

    bool has_set_host() const {
        return vtable_.size >=
               offsetof(pime_native_plugin, set_host) + sizeof(vtable_.set_host);
    }

    HostCallContext make_context() const {
        HostCallContext context;
        context.alias = alias_;
        context.generation = generation_;
        context.session_id = session_id_;
        context.secure = secure_;
        context.host_caps = host_caps_;
        context.caps = caps_;
        context.instance = const_cast<NativeInstance*>(this);
        return context;
    }

    static pime_status host_check(void* ud, const char* capability,
                                  uint32_t len) {
        auto* adapter = static_cast<NativeHostAdapter*>(ud);
        if (!adapter || !adapter->owner) return PIME_INVALID;
        if (len && !capability) return PIME_INVALID;
        const std::string name(capability ? capability : "", len);
        return adapter->owner->bridge_->check(adapter->owner->make_context(),
                                              name);
    }

    static pime_status host_invoke(void* ud, uint32_t kind, uint32_t op,
                                   const uint8_t* in, uint32_t in_len,
                                   uint8_t* out, uint32_t cap,
                                   uint32_t* out_len) {
        auto* adapter = static_cast<NativeHostAdapter*>(ud);
        if (!adapter || !adapter->owner || !out_len) return PIME_INVALID;
        std::vector<uint8_t> response;
        pime_status st = adapter->owner->bridge_->invoke(
            adapter->owner->make_context(), static_cast<HostKind>(kind),
            static_cast<uint8_t>(op), in, in_len, &response);
        *out_len = static_cast<uint32_t>(response.size());
        if (response.size() > cap) return PIME_BUFFER_TOO_SMALL;
        if (!response.empty()) {
            if (!out) return PIME_INVALID;
            std::copy(response.begin(), response.end(), out);
        }
        return st;
    }

    static pime_status host_emit(void* ud, uint64_t session_id,
                                 const uint8_t* data, uint32_t len) {
        auto* adapter = static_cast<NativeHostAdapter*>(ud);
        if (!adapter || !adapter->owner) return PIME_INVALID;
        return adapter->owner->bridge_->emit(
            session_id, adapter->owner->alias_, adapter->owner->generation_,
            data, len);
    }

    pime_status call(Call fn, const uint8_t* in, uint32_t in_len,
                     std::vector<uint8_t>* out) {
        uint32_t capacity = max_frame_ ? max_frame_ : 65536;
        if (capacity < 1024) capacity = 1024;
        out->resize(capacity);
        uint32_t out_len = 0;
        pime_status st = fn(state_, in, in_len, out->data(), capacity, &out_len);
        if (st == PIME_BUFFER_TOO_SMALL) {
            if (out_len > 4u * 1024u * 1024u) return PIME_FULL;
            out->resize(out_len);
            st = fn(state_, in, in_len, out->data(), out_len, &out_len);
        }
        if (st != PIME_OK) return st;
        if (out_len > out->size()) return PIME_CORRUPT;
        out->resize(out_len);
        return PIME_OK;
    }

    platform::Library library_;
    pime_native_plugin vtable_{};
    const cap::PluginCapContext* caps_ = nullptr;
    HostBridge* bridge_ = nullptr;
    std::string alias_;
    uint64_t generation_ = 0;
    uint32_t max_frame_ = 0;
    void* state_ = nullptr;
    std::unique_ptr<NativeHostAdapter> host_;
    uint64_t session_id_ = 0;
    bool secure_ = false;
    uint32_t host_caps_ = 0;
};

}  // namespace

pime_status create_instance(const codec::Manifest& manifest,
                            const std::string& package_dir,
                            const cap::PluginCapContext* caps,
                            HostBridge* bridge, const std::string& alias,
                            uint64_t generation,
                            std::unique_ptr<PluginInstance>* out) {
    if (!out) return PIME_INVALID;

    if (manifest.runtime == "native") {
        if (!manifest.trusted) return PIME_DENIED;
        static const char* kNames[] = {"plugin.dll", "plugin.so", "plugin.dylib"};
        std::string path;
        for (const char* name : kNames) {
            const std::string candidate = platform::path_join(package_dir, name);
            if (platform::path_exists(candidate)) {
                path = candidate;
                break;
            }
        }
        if (path.empty()) return PIME_NOT_FOUND;

        platform::Library library;
        std::string error;
        pime_status st = library.open(path, &error);
        if (st != PIME_OK) return st;
        auto query = reinterpret_cast<pime_status (*)(uint32_t, pime_native_plugin*)>(
            library.symbol("pime_native_plugin_query"));
        if (!query) return PIME_CORRUPT;
        pime_native_plugin vtable{};
        vtable.size = sizeof(vtable);
        vtable.version = PIME_ABI_MAJOR;
        st = query(PIME_ABI_MAJOR, &vtable);
        if (st != PIME_OK) return st;
        if (!vtable.init || !vtable.process) return PIME_CORRUPT;
        *out = std::make_unique<NativeInstance>(
            std::move(library), vtable, caps, bridge, alias, generation,
            manifest.limits.max_frame_bytes);
        return PIME_OK;
    }

    if (manifest.runtime == "wasm") {
#if defined(PIME_BUILD_WASM)
        return create_wasm_instance(manifest, package_dir, caps, bridge, alias,
                                    generation, out);
#else
        return PIME_UNSUPPORTED;
#endif
    }
    if (manifest.runtime == "lua") {
#if defined(PIME_BUILD_LUA)
        return create_lua_instance(manifest, package_dir, caps, bridge, alias,
                                   generation, out);
#else
        return PIME_UNSUPPORTED;
#endif
    }
    return PIME_UNSUPPORTED;
}

}  // namespace pime::host
