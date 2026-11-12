/*
 * WAMR-backed WASM plugin runtime. Third-party headers are confined to this
 * directory; everything crossing into the core goes through PluginInstance.
 *
 * Data model: frames and capability requests/responses are CBOR, exactly as in
 * the native and Lua runtimes. The guest gets a scratch region for input and
 * one pooled output buffer per instance; both are re-used across calls.
 */
#include "plugin_instance.hpp"

#include <wasm_export.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "capabilities/cap_context.hpp"
#include "codec/cbor.hpp"
#include "platform/platform.hpp"

namespace pime::host {

class WasmInstance;

WasmInstance* own(wasm_exec_env_t env);

namespace {

using codec::Value;

constexpr uint32_t kMinimumOut = 64 * 1024;
constexpr uint32_t kMaxFrame = 16 * 1024 * 1024;

std::once_flag g_once;
extern NativeSymbol g_symbols[];
constexpr uint32_t g_symbol_count = 36;

}  // namespace

class WasmInstance : public PluginInstance {
public:
    WasmInstance(std::string dir, const cap::PluginCapContext* caps,
                 HostBridge* bridge, std::string alias, uint64_t generation,
                 uint32_t max_frame)
        : dir_(std::move(dir)),
          caps_(caps),
          bridge_(bridge),
          alias_(std::move(alias)),
          generation_(generation),
          max_frame_(max_frame ? max_frame : kMinimumOut) {}

    ~WasmInstance() override { shutdown(); }

    pime_status init(const std::vector<uint8_t>& config) override {
        std::call_once(g_once, [] {
            wasm_runtime_init();
            wasm_runtime_register_natives(
                "env", g_symbols, g_symbol_count);
        });

        std::vector<uint8_t> bytes;
        const std::string path = platform::path_join(dir_, "plugin.wasm");
        pime_status st = platform::read_file(path, &bytes);
        if (st != PIME_OK) return st;
        if (bytes.empty()) return PIME_CORRUPT;

        char error[256] = {};
        module_ = wasm_runtime_load(bytes.data(), static_cast<uint32_t>(bytes.size()),
                                    error, sizeof(error));
        if (!module_) return PIME_CORRUPT;

        instance_ = wasm_runtime_instantiate(module_, 64 * 1024,
                                             8 * 1024 * 1024, error,
                                             sizeof(error));
        if (!instance_) {
            wasm_runtime_unload(module_);
            module_ = nullptr;
            return PIME_CORRUPT;
        }
        wasm_runtime_set_user_data(instance_, this);

        func_init_ = wasm_runtime_lookup_function(instance_, "pime_init");
        func_process_ = wasm_runtime_lookup_function(instance_, "pime_process");
        func_event_ = wasm_runtime_lookup_function(instance_, "pime_on_event");
        func_shutdown_ = wasm_runtime_lookup_function(instance_, "pime_shutdown");
        func_scratch_ptr_ =
            wasm_runtime_lookup_function(instance_, "pime_scratch_ptr");
        func_scratch_cap_ =
            wasm_runtime_lookup_function(instance_, "pime_scratch_cap");
        if (!func_init_ || !func_process_) return PIME_CORRUPT;

        exec_env_ = wasm_runtime_create_exec_env(func_process_, 64 * 1024);
        if (!exec_env_) return PIME_NO_MEMORY;

        scratch_ptr_ = call_u32(func_scratch_ptr_);
        scratch_cap_ = call_u32(func_scratch_cap_);

        out_cap_ = std::min(std::max(max_frame_, kMinimumOut), kMaxFrame);
        out_offset_ = module_malloc(out_cap_);
        out_len_offset_ = module_malloc(sizeof(uint32_t));
        if (!out_offset_ || !out_len_offset_) return PIME_NO_MEMORY;

        const uint32_t config_offset =
            copy_in(config.data(), static_cast<uint32_t>(config.size()));
        if (!config_offset && !config.empty()) return PIME_NO_MEMORY;
        uint32_t argv[2] = {config_offset, static_cast<uint32_t>(config.size())};
        if (!wasm_runtime_call_wasm(exec_env_, func_init_, 2, argv))
            return PIME_CORRUPT;
        return static_cast<pime_status>(argv[0]);
    }

    pime_status process(const uint8_t* in, uint32_t in_len,
                        std::vector<uint8_t>* out) override {
        return call_frame(func_process_, in, in_len, out);
    }

    pime_status on_event(const uint8_t* evt, uint32_t evt_len,
                         std::vector<uint8_t>* out) override {
        if (!func_event_) return PIME_UNSUPPORTED;
        return call_frame(func_event_, evt, evt_len, out);
    }

    void set_session(uint64_t session_id, bool secure,
                     uint32_t host_caps) override {
        session_id_ = session_id;
        secure_ = secure;
        host_caps_ = host_caps;
    }

    void shutdown() override {
        if (instance_ && exec_env_ && func_shutdown_) {
            uint32_t argv[1] = {0};
            wasm_runtime_call_wasm(exec_env_, func_shutdown_, 0, argv);
        }
        if (exec_env_) {
            wasm_runtime_destroy_exec_env(exec_env_);
            exec_env_ = nullptr;
        }
        if (instance_) {
            wasm_runtime_deinstantiate(instance_);
            instance_ = nullptr;
        }
        if (module_) {
            wasm_runtime_unload(module_);
            module_ = nullptr;
        }
    }

    bool alive() const override { return instance_ != nullptr; }

    /* ---- bridge helpers used by host imports --------------------------- */

    HostCallContext context() const {
        HostCallContext call;
        call.alias = alias_;
        call.generation = generation_;
        call.session_id = session_id_;
        call.secure = secure_;
        call.host_caps = host_caps_;
        call.caps = caps_;
        call.instance = const_cast<WasmInstance*>(this);
        return call;
    }

    pime_status invoke_raw(HostKind kind, uint8_t op, const uint8_t* in,
                           uint32_t in_len, std::vector<uint8_t>* reply) {
        return bridge_->invoke(context(), kind, op, in, in_len, reply);
    }

    /* Invokes and decodes {status, result, error}. */
    pime_status invoke_value(HostKind kind, uint8_t op, const Value& request,
                             Value* result) {
        std::vector<uint8_t> encoded;
        codec::encode(request, &encoded);
        std::vector<uint8_t> reply;
        pime_status st = invoke_raw(kind, op, encoded.data(),
                                    static_cast<uint32_t>(encoded.size()),
                                    &reply);
        *result = Value::null();
        Value envelope;
        if (!reply.empty() &&
            codec::decode(reply.data(), reply.size(), &envelope) == PIME_OK) {
            if (const Value* status = envelope.find("status");
                status && status->is_number())
                st = static_cast<pime_status>(status->as_uint());
            if (const Value* value = envelope.find("result"))
                *result = *value;
        }
        return st;
    }

    uint32_t id_from(HostKind kind, uint8_t op, const Value& request,
                     const char* field) {
        Value result;
        if (invoke_value(kind, op, request, &result) != PIME_OK) return 0;
        const Value* value = result.find(field);
        return static_cast<uint32_t>(
            value && value->is_number() ? value->as_uint() : result.as_uint());
    }

    /* Copies a reply envelope into the guest buffer. */
    static pime_status copy_envelope(const std::vector<uint8_t>& reply,
                                     uint8_t* out, uint32_t cap,
                                     uint32_t* out_len, pime_status status) {
        if (out_len) *out_len = static_cast<uint32_t>(reply.size());
        if (reply.size() > cap) return PIME_BUFFER_TOO_SMALL;
        if (!reply.empty()) {
            if (!out) return PIME_INVALID;
            std::memcpy(out, reply.data(), reply.size());
        }
        return status;
    }

    pime_status emit(uint64_t session_id, const uint8_t* data, uint32_t len) {
        return bridge_->emit(session_id, alias_, generation_, data, len);
    }

    static WasmInstance* from(wasm_exec_env_t env) { return own(env); }

private:
    pime_status call_frame(wasm_function_inst_t func, const uint8_t* in,
                           uint32_t in_len, std::vector<uint8_t>* out) {
        if (!instance_ || !exec_env_ || !func) return PIME_CLOSED;
        uint64_t in_offset = 0;
        bool allocated = false;
        if (in_len && in) {
            if (scratch_ptr_ && scratch_cap_ >= in_len) {
                std::memcpy(
                    wasm_runtime_addr_app_to_native(instance_, scratch_ptr_), in,
                    in_len);
                in_offset = scratch_ptr_;
            } else {
                in_offset = module_malloc(in_len);
                if (!in_offset) return PIME_NO_MEMORY;
                allocated = true;
                std::memcpy(
                    wasm_runtime_addr_app_to_native(instance_, in_offset), in,
                    in_len);
            }
        }
        set_out_len(0);
        uint32_t argv[5] = {static_cast<uint32_t>(in_offset), in_len,
                            static_cast<uint32_t>(out_offset_), out_cap_,
                            static_cast<uint32_t>(out_len_offset_)};
        bool ok = wasm_runtime_call_wasm(exec_env_, func, 5, argv);
        pime_status st = ok ? static_cast<pime_status>(argv[0]) : PIME_CORRUPT;

        if (st == PIME_BUFFER_TOO_SMALL) {
            const uint32_t needed = out_len();
            if (needed > kMaxFrame) {
                st = PIME_FULL;
            } else if (needed > out_cap_) {
                wasm_runtime_module_free(instance_, out_offset_);
                out_cap_ = needed;
                out_offset_ = module_malloc(out_cap_);
                if (!out_offset_) {
                    st = PIME_NO_MEMORY;
                } else {
                    argv[3] = out_cap_;
                    ok = wasm_runtime_call_wasm(exec_env_, func, 5, argv);
                    st = ok ? static_cast<pime_status>(argv[0]) : PIME_CORRUPT;
                }
            }
        }

        if (allocated) wasm_runtime_module_free(instance_, in_offset);
        if (st != PIME_OK) return st;

        const uint32_t length = out_len();
        if (length > out_cap_) return PIME_CORRUPT;
        const uint8_t* data = static_cast<const uint8_t*>(
            wasm_runtime_addr_app_to_native(instance_, out_offset_));
        out->assign(data, data + length);
        return PIME_OK;
    }

    uint64_t module_malloc(uint32_t size) {
        if (!instance_) return 0;
        void* native = nullptr;
        return wasm_runtime_module_malloc(instance_, size, &native);
    }

    uint32_t copy_in(const uint8_t* data, uint32_t len) {
        if (!len || !data) return 0;
        if (scratch_ptr_ && scratch_cap_ >= len) {
            std::memcpy(wasm_runtime_addr_app_to_native(instance_, scratch_ptr_),
                        data, len);
            return static_cast<uint32_t>(scratch_ptr_);
        }
        const uint64_t offset = module_malloc(len);
        if (!offset) return 0;
        std::memcpy(wasm_runtime_addr_app_to_native(instance_, offset), data,
                    len);
        return static_cast<uint32_t>(offset);
    }

    void set_out_len(uint32_t value) {
        if (!instance_ || !out_len_offset_) return;
        *reinterpret_cast<uint32_t*>(
            wasm_runtime_addr_app_to_native(instance_, out_len_offset_)) = value;
    }

    uint32_t out_len() const {
        if (!instance_ || !out_len_offset_) return 0;
        return *reinterpret_cast<const uint32_t*>(
            wasm_runtime_addr_app_to_native(instance_, out_len_offset_));
    }

    uint32_t call_u32(wasm_function_inst_t func) {
        if (!func || !instance_) return 0;
        wasm_exec_env_t env = wasm_runtime_create_exec_env(func, 16 * 1024);
        if (!env) return 0;
        uint32_t argv[1] = {0};
        const bool ok = wasm_runtime_call_wasm(env, func, 0, argv);
        wasm_runtime_destroy_exec_env(env);
        return ok ? argv[0] : 0;
    }

    std::string dir_;
    const cap::PluginCapContext* caps_ = nullptr;
    HostBridge* bridge_ = nullptr;
    std::string alias_;
    uint64_t generation_ = 0;
    uint32_t max_frame_ = 0;

    wasm_module_t module_ = nullptr;
    wasm_module_inst_t instance_ = nullptr;
    wasm_exec_env_t exec_env_ = nullptr;
    wasm_function_inst_t func_init_ = nullptr;
    wasm_function_inst_t func_process_ = nullptr;
    wasm_function_inst_t func_event_ = nullptr;
    wasm_function_inst_t func_shutdown_ = nullptr;
    wasm_function_inst_t func_scratch_ptr_ = nullptr;
    wasm_function_inst_t func_scratch_cap_ = nullptr;

    uint64_t scratch_ptr_ = 0;
    uint32_t scratch_cap_ = 0;
    uint64_t out_offset_ = 0;
    uint64_t out_len_offset_ = 0;
    uint32_t out_cap_ = 0;

    uint64_t session_id_ = 0;
    bool secure_ = false;
    uint32_t host_caps_ = 0;
};

WasmInstance* own(wasm_exec_env_t env) {
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(env);
    return instance ? static_cast<WasmInstance*>(
                          wasm_runtime_get_user_data(instance))
                    : nullptr;
}

namespace {

uint8_t crypto_op(const Value& request) {
    const Value* name = request.find("op");
    if (!name || !name->is_text()) return 0;
    const std::string_view op = name->as_text();
    if (op == "mac") return 1;
    if (op == "encrypt" || op == "aead_encrypt" || op == "cipher") return 2;
    if (op == "decrypt" || op == "aead_decrypt") return 3;
    if (op == "random") return 4;
    if (op == "verify") return 5;
    if (op == "sign") return 6;
    if (op == "kdf") return 7;
    if (op == "x509_verify" || op == "x509") return 8;
    return 0;
}

uint8_t audio_op(const Value& request) {
    const Value* name = request.find("op");
    if (!name || !name->is_text()) return 0;
    const std::string_view op = name->as_text();
    if (op == "capture_stop") return 1;
    if (op == "capture_read") return 2;
    if (op == "playback_start" || op == "play") return 3;
    if (op == "playback_stop") return 4;
    if (op == "playback_write") return 5;
    if (op == "convert" || op == "pcm_convert") return 6;
    if (op == "opus_encode") return 7;
    if (op == "opus_decode") return 8;
    return 0;
}

uint32_t forward_op(wasm_exec_env_t env, HostKind kind, const uint8_t* op,
                    const uint8_t* in, uint32_t in_len, uint8_t* out,
                    uint32_t cap, uint32_t* out_len) {
    WasmInstance* self = own(env);
    if (!self || !op) return PIME_INVALID;
    std::vector<uint8_t> reply;
    const pime_status st = self->invoke_raw(kind, *op, in, in_len, &reply);
    return WasmInstance::copy_envelope(reply, out, cap, out_len, st);
}

/* ---- net ------------------------------------------------------------- */

uint32_t h_net_request(wasm_exec_env_t env, const uint8_t* req, uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return 0;
    Value request = Value::map_v();
    if (len && codec::decode(req, len, &request) != PIME_OK) return 0;
    return self->id_from(HostKind::Net, static_cast<uint8_t>(NetOp::Http),
                         request, "request");
}

uint32_t h_net_result(wasm_exec_env_t env, uint32_t id, uint8_t* out,
                      uint32_t cap) {
    WasmInstance* self = own(env);
    if (!self) return 0;
    Value request = Value::map_v();
    request.set("request", Value::uint_v(id));
    std::vector<uint8_t> encoded;
    codec::encode(request, &encoded);
    std::vector<uint8_t> reply;
    const pime_status st = self->invoke_raw(
        HostKind::Net, static_cast<uint8_t>(NetOp::Result), encoded.data(),
        static_cast<uint32_t>(encoded.size()), &reply);
    uint32_t out_len = 0;
    WasmInstance::copy_envelope(reply, out, cap, &out_len, st);
    return out_len;
}

uint32_t h_net_open(wasm_exec_env_t env, const char* proto, const char* url,
                    uint32_t url_len) {
    WasmInstance* self = own(env);
    if (!self || !proto) return 0;
    Value request = Value::map_v();
    request.set("proto", Value::text_v(proto));
    request.set("url", Value::text_v(std::string_view(url ? url : "", url_len)));
    return self->id_from(HostKind::Net, static_cast<uint8_t>(NetOp::Open),
                         request, "id");
}

uint32_t h_net_send(wasm_exec_env_t env, uint32_t id, const uint8_t* data,
                    uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("id", Value::uint_v(id));
    request.set("data",
                Value::bytes(std::string_view(reinterpret_cast<const char*>(data),
                                              len)));
    Value result;
    return self->invoke_value(HostKind::Net,
                              static_cast<uint8_t>(NetOp::Send), request,
                              &result);
}

uint32_t h_net_close(wasm_exec_env_t env, uint32_t id) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("id", Value::uint_v(id));
    Value result;
    return self->invoke_value(HostKind::Net,
                              static_cast<uint8_t>(NetOp::Close), request,
                              &result);
}

/* ---- crypto / audio -------------------------------------------------- */

/* Returns the encoded length of the CBOR item starting at p. */
size_t cbor_item_size(const uint8_t* p) {
    if (!p) return 0;
    const uint8_t header = p[0];
    const uint8_t major = header >> 5;
    const uint8_t add = header & 31;
    size_t head = 1;
    uint64_t value = add;
    if (add == 24) {
        head = 2;
        value = p[1];
    } else if (add == 25) {
        head = 3;
        value = (uint64_t(p[1]) << 8) | p[2];
    } else if (add == 26) {
        head = 5;
        value = (uint64_t(p[1]) << 24) | (uint64_t(p[2]) << 16) |
                (uint64_t(p[3]) << 8) | p[4];
    } else if (add == 27) {
        head = 9;
        value = 0;
        for (int i = 1; i < 9; ++i) value = (value << 8) | p[i];
    }
    if (major == 2 || major == 3) return head + static_cast<size_t>(value);
    if (major == 6) return head + cbor_item_size(p + head);
    if (major == 4) {
        size_t total = head;
        for (uint64_t i = 0; i < value; ++i)
            total += cbor_item_size(p + total);
        return total;
    }
    if (major == 5) {
        size_t total = head;
        for (uint64_t i = 0; i < value * 2; ++i)
            total += cbor_item_size(p + total);
        return total;
    }
    return head;
}

pime_status op_request(HostKind kind, bool crypto, wasm_exec_env_t env,
                       const uint8_t* req, uint8_t* out, uint32_t cap,
                       uint32_t* out_len) {
    WasmInstance* self = own(env);
    if (!self || !req) return PIME_INVALID;
    const size_t length = cbor_item_size(req);
    if (!length) return PIME_INVALID;
    Value request;
    if (codec::decode(req, length, &request) != PIME_OK) return PIME_INVALID;
    const uint8_t op = crypto ? crypto_op(request) : audio_op(request);
    std::vector<uint8_t> reply;
    const pime_status st = self->invoke_raw(
        kind, op, req, static_cast<uint32_t>(length), &reply);
    return WasmInstance::copy_envelope(reply, out, cap, out_len, st);
}

pime_status h_crypto(wasm_exec_env_t env, const uint8_t* req, uint8_t* out,
                     uint32_t cap, uint32_t* out_len) {
    return op_request(HostKind::Crypto, true, env, req, out, cap, out_len);
}

pime_status h_audio(wasm_exec_env_t env, const uint8_t* req, uint8_t* out,
                    uint32_t cap, uint32_t* out_len) {
    return op_request(HostKind::Audio, false, env, req, out, cap, out_len);
}

/* ---- ml -------------------------------------------------------------- */

uint32_t h_ml_load(wasm_exec_env_t env, const char* model_id, uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return 0;
    Value request = Value::map_v();
    request.set("model", Value::text_v(
                             std::string_view(model_id ? model_id : "", len)));
    return self->id_from(HostKind::Ml, static_cast<uint8_t>(MlOp::Load), request,
                         "session");
}

pime_status h_ml_load_ex(wasm_exec_env_t env, const uint8_t* desc, uint32_t len,
                         uint32_t* session) {
    WasmInstance* self = own(env);
    if (!self || !session) return PIME_INVALID;
    Value request = Value::map_v();
    if (len && codec::decode(desc, len, &request) != PIME_OK)
        return PIME_INVALID;
    Value* model_id = request.find("model_id");
    if (model_id && !request.has("model")) request.set("model", *model_id);
    Value result;
    const pime_status st = self->invoke_value(
        HostKind::Ml, static_cast<uint8_t>(MlOp::Load), request, &result);
    const Value* value = result.find("session");
    *session = static_cast<uint32_t>(value ? value->as_uint() : 0);
    return st;
}

pime_status h_ml_run(wasm_exec_env_t env, uint32_t id, const uint8_t* in,
                     uint32_t in_len, uint8_t* out, uint32_t cap,
                     uint32_t* out_len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("session", Value::uint_v(id));
    request.set("in",
                Value::bytes(std::string_view(reinterpret_cast<const char*>(in),
                                              in_len)));
    std::vector<uint8_t> encoded;
    codec::encode(request, &encoded);
    std::vector<uint8_t> reply;
    const pime_status st = self->invoke_raw(
        HostKind::Ml, static_cast<uint8_t>(MlOp::Run), encoded.data(),
        static_cast<uint32_t>(encoded.size()), &reply);
    return WasmInstance::copy_envelope(reply, out, cap, out_len, st);
}

uint32_t h_ml_run_async(wasm_exec_env_t env, uint32_t id, const uint8_t* in,
                        uint32_t in_len) {
    WasmInstance* self = own(env);
    if (!self) return 0;
    Value request = Value::map_v();
    request.set("session", Value::uint_v(id));
    request.set("in",
                Value::bytes(std::string_view(reinterpret_cast<const char*>(in),
                                              in_len)));
    return self->id_from(HostKind::Ml, static_cast<uint8_t>(MlOp::RunAsync),
                         request, "job");
}

uint32_t h_ml_cancel(wasm_exec_env_t env, uint32_t job) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("job", Value::uint_v(job));
    Value result;
    return self->invoke_value(HostKind::Ml, static_cast<uint8_t>(MlOp::Cancel),
                              request, &result);
}

uint32_t h_ml_free(wasm_exec_env_t env, uint32_t id) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("session", Value::uint_v(id));
    Value result;
    return self->invoke_value(HostKind::Ml, static_cast<uint8_t>(MlOp::Free),
                              request, &result);
}

/* ---- log / metric / time / random / timer / caps --------------------- */

uint32_t h_log(wasm_exec_env_t env, uint32_t level, const char* tag,
               uint32_t tag_len, const char* msg, uint32_t msg_len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("level", Value::uint_v(level));
    request.set("tag", Value::text_v(
                           std::string_view(tag ? tag : "", tag_len)));
    request.set("message", Value::text_v(
                               std::string_view(msg ? msg : "", msg_len)));
    Value result;
    return self->invoke_value(HostKind::Log, 0, request, &result);
}

uint64_t h_time(wasm_exec_env_t env, uint32_t kind) {
    WasmInstance* self = own(env);
    if (!self) return 0;
    Value request = Value::map_v();
    request.set("kind", Value::uint_v(kind));
    Value result;
    if (self->invoke_value(HostKind::Time, 0, request, &result) != PIME_OK)
        return 0;
    const Value* value = result.find("value");
    return value && value->is_number() ? value->as_uint() : result.as_uint();
}

uint32_t h_random(wasm_exec_env_t env, uint8_t* out, uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    if (!out && len) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("len", Value::uint_v(len));
    Value result;
    const pime_status st =
        self->invoke_value(HostKind::Random, 0, request, &result);
    if (st != PIME_OK) return st;
    const std::string_view bytes = result.as_bytes();
    if (bytes.size() > len) return PIME_BUFFER_TOO_SMALL;
    std::memcpy(out, bytes.data(), bytes.size());
    return PIME_OK;
}

uint32_t h_timer_start(wasm_exec_env_t env, uint32_t delay_ms, uint32_t repeat,
                       const uint8_t* id, uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return 0;
    Value request = Value::map_v();
    if (repeat) {
        request.set("interval_ms", Value::uint_v(delay_ms));
    } else {
        request.set("delay_ms", Value::uint_v(delay_ms));
    }
    if (id && len)
        request.set("event_id",
                    Value::text_v(std::string_view(
                        reinterpret_cast<const char*>(id), len)));
    const uint8_t op = repeat ? static_cast<uint8_t>(TimerOp::Every)
                              : static_cast<uint8_t>(TimerOp::After);
    return self->id_from(HostKind::Timer, op, request, "id");
}

uint32_t h_timer_cancel(wasm_exec_env_t env, uint32_t id) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("id", Value::uint_v(id));
    Value result;
    return self->invoke_value(HostKind::Timer,
                              static_cast<uint8_t>(TimerOp::Cancel), request,
                              &result);
}

uint32_t h_caps(wasm_exec_env_t env, uint8_t* out, uint32_t cap,
                uint32_t* out_len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    std::vector<uint8_t> reply;
    const pime_status st = self->invoke_raw(
        HostKind::Caps, static_cast<uint8_t>(CapsOp::Query), nullptr, 0, &reply);
    return WasmInstance::copy_envelope(reply, out, cap, out_len, st);
}

/* ---- threads --------------------------------------------------------- */

uint32_t h_thread_create(wasm_exec_env_t env, const uint8_t* desc, uint32_t len,
                         uint32_t* tid) {
    WasmInstance* self = own(env);
    if (!self || !tid) return PIME_INVALID;
    Value request = Value::map_v();
    if (len && codec::decode(desc, len, &request) != PIME_OK)
        return PIME_INVALID;
    const pime_status st = self->invoke_value(
        HostKind::Thread, static_cast<uint8_t>(ThreadOp::Create), request,
        &request);
    const Value* value = request.find("tid");
    *tid = static_cast<uint32_t>(value ? value->as_uint() : 0);
    return st;
}

uint32_t h_thread_post(wasm_exec_env_t env, uint32_t tid, const uint8_t* msg,
                       uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    request.set("data",
                Value::bytes(std::string_view(reinterpret_cast<const char*>(msg),
                                              len)));
    Value result;
    return self->invoke_value(HostKind::Thread,
                              static_cast<uint8_t>(ThreadOp::Post), request,
                              &result);
}

uint32_t h_thread_recv(wasm_exec_env_t env, uint32_t tid, uint8_t* out,
                       uint32_t cap, uint32_t* out_len, uint32_t timeout_ms) {
    WasmInstance* self = own(env);
    if (!self || !out_len) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    request.set("timeout_ms", Value::uint_v(timeout_ms));
    Value result;
    const pime_status st = self->invoke_value(
        HostKind::Thread, static_cast<uint8_t>(ThreadOp::Recv), request,
        &result);
    if (st != PIME_OK) return st;
    const std::string_view bytes = result.as_bytes();
    *out_len = static_cast<uint32_t>(bytes.size());
    if (bytes.size() > cap) return PIME_BUFFER_TOO_SMALL;
    if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
    return PIME_OK;
}

uint32_t h_thread_park(wasm_exec_env_t env, uint32_t tid, uint32_t timeout_ms) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    request.set("timeout_ms", Value::uint_v(timeout_ms));
    Value result;
    return self->invoke_value(HostKind::Thread,
                              static_cast<uint8_t>(ThreadOp::Park), request,
                              &result);
}

uint32_t h_thread_unpark(wasm_exec_env_t env, uint32_t tid) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    Value result;
    return self->invoke_value(HostKind::Thread,
                              static_cast<uint8_t>(ThreadOp::Unpark), request,
                              &result);
}

uint32_t h_thread_close(wasm_exec_env_t env, uint32_t tid) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    Value result;
    return self->invoke_value(HostKind::Thread,
                              static_cast<uint8_t>(ThreadOp::Close), request,
                              &result);
}

uint32_t h_thread_join(wasm_exec_env_t env, uint32_t tid, uint32_t timeout_ms) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    request.set("timeout_ms", Value::uint_v(timeout_ms));
    Value result;
    return self->invoke_value(HostKind::Thread,
                              static_cast<uint8_t>(ThreadOp::Join), request,
                              &result);
}

uint32_t h_thread_info(wasm_exec_env_t env, uint32_t tid, uint8_t* out,
                       uint32_t cap, uint32_t* out_len) {
    WasmInstance* self = own(env);
    if (!self || !out_len) return PIME_INVALID;
    Value request = Value::map_v();
    request.set("tid", Value::uint_v(tid));
    Value result;
    const pime_status st = self->invoke_value(
        HostKind::Thread, static_cast<uint8_t>(ThreadOp::Info), request,
        &result);
    if (st != PIME_OK) return st;
    const std::vector<uint8_t> encoded = codec::encode_to_vector(result);
    *out_len = static_cast<uint32_t>(encoded.size());
    if (encoded.size() > cap) return PIME_BUFFER_TOO_SMALL;
    if (!encoded.empty()) std::memcpy(out, encoded.data(), encoded.size());
    return PIME_OK;
}

uint32_t h_emit(wasm_exec_env_t env, uint64_t session_id, const uint8_t* data,
                uint32_t len) {
    WasmInstance* self = own(env);
    if (!self) return PIME_INVALID;
    return self->emit(session_id, data, len);
}

/* ---- namespace tables ------------------------------------------------ */

uint32_t h_workspace(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
                     uint32_t in_len, uint8_t* out, uint32_t cap,
                     uint32_t* out_len) {
    return forward_op(env, HostKind::Workspace, op, in, in_len, out, cap,
                      out_len);
}
uint32_t h_store(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
                 uint32_t in_len, uint8_t* out, uint32_t cap,
                 uint32_t* out_len) {
    return forward_op(env, HostKind::Store, op, in, in_len, out, cap, out_len);
}
uint32_t h_asset(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
                 uint32_t in_len, uint8_t* out, uint32_t cap,
                 uint32_t* out_len) {
    return forward_op(env, HostKind::Asset, op, in, in_len, out, cap, out_len);
}
uint32_t h_config(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
                  uint32_t in_len, uint8_t* out, uint32_t cap,
                  uint32_t* out_len) {
    return forward_op(env, HostKind::Config, op, in, in_len, out, cap, out_len);
}
uint32_t h_ctx(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
               uint32_t in_len, uint8_t* out, uint32_t cap,
               uint32_t* out_len) {
    return forward_op(env, HostKind::Ctx, op, in, in_len, out, cap, out_len);
}
uint32_t h_channel(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
                   uint32_t in_len, uint8_t* out, uint32_t cap,
                   uint32_t* out_len) {
    return forward_op(env, HostKind::Channel, op, in, in_len, out, cap,
                      out_len);
}
uint32_t h_sync(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
                uint32_t in_len, uint8_t* out, uint32_t cap,
                uint32_t* out_len) {
    return forward_op(env, HostKind::Sync, op, in, in_len, out, cap, out_len);
}
uint32_t h_shm(wasm_exec_env_t env, const uint8_t* op, const uint8_t* in,
               uint32_t in_len, uint8_t* out, uint32_t cap,
               uint32_t* out_len) {
    return forward_op(env, HostKind::Shm, op, in, in_len, out, cap, out_len);
}

NativeSymbol g_symbols[] = {
    {"pime_host_net_request", (void*)h_net_request, "i(*i)", nullptr},
    {"pime_host_net_result", (void*)h_net_result, "i(i*i)", nullptr},
    {"pime_host_net_open", (void*)h_net_open, "i(**i)", nullptr},
    {"pime_host_net_send", (void*)h_net_send, "i(i*i)", nullptr},
    {"pime_host_net_close", (void*)h_net_close, "i(i)", nullptr},
    {"pime_host_crypto", (void*)h_crypto, "i(**i*)", nullptr},
    {"pime_host_audio", (void*)h_audio, "i(**i*)", nullptr},
    {"pime_host_ml_load", (void*)h_ml_load, "i(*i)", nullptr},
    {"pime_host_ml_load_ex", (void*)h_ml_load_ex, "i(*i*)", nullptr},
    {"pime_host_ml_run", (void*)h_ml_run, "i(i*i*i*)", nullptr},
    {"pime_host_ml_run_async", (void*)h_ml_run_async, "i(i*i)", nullptr},
    {"pime_host_ml_cancel", (void*)h_ml_cancel, "i(i)", nullptr},
    {"pime_host_ml_free", (void*)h_ml_free, "i(i)", nullptr},
    {"pime_host_log", (void*)h_log, "i(i*i*i)", nullptr},
    {"pime_host_workspace", (void*)h_workspace, "i(**i*i*)", nullptr},
    {"pime_host_store", (void*)h_store, "i(**i*i*)", nullptr},
    {"pime_host_asset", (void*)h_asset, "i(**i*i*)", nullptr},
    {"pime_host_config", (void*)h_config, "i(**i*i*)", nullptr},
    {"pime_host_ctx", (void*)h_ctx, "i(**i*i*)", nullptr},
    {"pime_host_channel", (void*)h_channel, "i(**i*i*)", nullptr},
    {"pime_host_sync", (void*)h_sync, "i(**i*i*)", nullptr},
    {"pime_host_shm", (void*)h_shm, "i(**i*i*)", nullptr},
    {"pime_host_time", (void*)h_time, "I(i)", nullptr},
    {"pime_host_random", (void*)h_random, "i(*i)", nullptr},
    {"pime_host_timer_start", (void*)h_timer_start, "i(ii*i)", nullptr},
    {"pime_host_timer_cancel", (void*)h_timer_cancel, "i(i)", nullptr},
    {"pime_host_caps", (void*)h_caps, "i(*i*)", nullptr},
    {"pime_host_thread_create", (void*)h_thread_create, "i(*i*)", nullptr},
    {"pime_host_thread_post", (void*)h_thread_post, "i(i*i)", nullptr},
    {"pime_host_thread_recv", (void*)h_thread_recv, "i(i*i*i)", nullptr},
    {"pime_host_thread_park", (void*)h_thread_park, "i(ii)", nullptr},
    {"pime_host_thread_unpark", (void*)h_thread_unpark, "i(i)", nullptr},
    {"pime_host_thread_close", (void*)h_thread_close, "i(i)", nullptr},
    {"pime_host_thread_join", (void*)h_thread_join, "i(ii)", nullptr},
    {"pime_host_thread_info", (void*)h_thread_info, "i(i*i*)", nullptr},
    {"pime_host_emit", (void*)h_emit, "i(I*i)", nullptr},
};

}  // namespace

pime_status create_wasm_instance(const codec::Manifest& manifest,
                                 const std::string& package_dir,
                                 const cap::PluginCapContext* caps,
                                 HostBridge* bridge, const std::string& alias,
                                 uint64_t generation,
                                 std::unique_ptr<PluginInstance>* out) {
    if (!out || !caps || !bridge) return PIME_INVALID;
    *out = std::make_unique<WasmInstance>(
        package_dir, caps, bridge, alias, generation,
        manifest.limits.max_frame_bytes);
    return PIME_OK;
}

}  // namespace pime::host
