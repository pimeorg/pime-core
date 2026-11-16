#ifndef PIME_CORE_CORE_HPP
#define PIME_CORE_CORE_HPP

#include <pime/abi.h>
#include <pime/hal.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "capabilities/asset/asset.hpp"
#include "capabilities/audio/stream.hpp"
#include "capabilities/caps/caps.hpp"
#include "capabilities/config/config.hpp"
#include "capabilities/log/log.hpp"
#include "capabilities/metric/metric.hpp"
#include "capabilities/ml/ml.hpp"
#include "capabilities/net/net.hpp"
#include "capabilities/permission/permission.hpp"
#include "capabilities/store/store.hpp"
#include "capabilities/timer/timer.hpp"
#include "capabilities/workspace/workspace.hpp"
#include "codec/cbor.hpp"
#include "codec/manifest.hpp"
#include "host/host_ops.hpp"
#include "host/package.hpp"
#include "host/plugin_instance.hpp"
#include "phl/buffer_pool.hpp"
#include "phl/channel.hpp"
#include "phl/context_cache.hpp"
#include "phl/handle.hpp"
#include "phl/shared_buffer.hpp"
#include "phl/sync.hpp"
#include "phl/thread.hpp"
#include "util/semver.hpp"

namespace pime::core {

struct CoreOptions {
    std::string plugin_dir;
    std::string data_dir;
    std::string backend_dir;
    std::string trust_store;
    std::string proxy;
    std::string ca_file;
    std::string client_cert;
    std::string client_key;
    std::vector<uint8_t> store_key;
    uint32_t log_level = PIME_LOG_INFO;
    uint32_t flags = 0;
    uint64_t memory_budget_mb = 0;
    pime_host_sink sink{};
    void* sink_ud = nullptr;
};

/* Per-session input state. Plugin business state is keyed by plugin alias. */
struct Session {
    uint64_t id = 0;
    uint32_t flags = 0;
    codec::Value context = codec::Value::map_v();
    std::string app_id;
    uint32_t field_type = PIME_FIELD_UNKNOWN;
    uint32_t input_type = PIME_INPUT_OTHER;
    std::string locale;
    uint32_t page_size = 5;
    uint32_t host_caps = 0;
    bool focused = true;
    bool secure = false;
    std::map<std::string, codec::Value> state;
    std::map<std::string, uint32_t> state_schema;
};

struct ChainItem {
    std::string alias;
    std::string plugin_id;
    std::string version;
    std::string code_hash;
    std::string package_dir;
    codec::Manifest manifest;
    cap::PluginCapContext caps;
    std::unique_ptr<host::PluginInstance> instance;
    bool enabled = true;
};

struct Generation {
    uint64_t id = 0;
    std::deque<ChainItem> chain;
    cap::ConfigSnapshot config;
    uint32_t workspace_bytes = 256 * 1024;
    bool retired = false;
};

struct InstalledPlugin {
    std::string id;
    std::string version;
    std::string dir;
    std::string code_hash;
    codec::Manifest manifest;
    bool enabled = true;
};

struct AsyncEvent {
    std::string alias;
    uint64_t generation = 0;
    codec::Value payload;
};

struct EmitEnvelope {
    uint64_t session_id = 0;
    std::string alias;
    uint64_t generation = 0;
    std::vector<uint8_t> data;
};

class Core final : public host::HostBridge {
public:
    explicit Core(const CoreOptions& options);
    ~Core() override;

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    pime_status initialize();

    void set_log_level(pime_log_level level);
    void set_plugin_log_level(const std::string& plugin_id, pime_log_level level);

    Session* create_session(const pime_session_config& config,
                            pime_status* status);
    void destroy_session(Session* session);
    Session* find_session(uint64_t id);

    pime_status key(Session* session, const pime_key_event& event, bool* handled);
    pime_status touch(Session* session, const pime_touch_event& event,
                      bool* handled);
    pime_status text(Session* session, const pime_text& text);
    pime_status candidate(Session* session, const pime_candidate_event& event);
    pime_status surrounding(Session* session, const pime_text& before,
                            const pime_text& after);
    pime_status context(Session* session, const pime_context& context);
    pime_status focus(Session* session, bool focused);
    pime_status secure(Session* session, bool secure);
    pime_status reset(Session* session);
    pime_status audio_in(Session* session, const pime_audio_frame& frame);
    pime_status voice(Session* session, uint32_t action);

    pime_status pause();
    pime_status resume();
    pime_status memory_warning(uint32_t level);
    pime_status network_changed(uint32_t state);
    pime_status audio_focus(uint32_t focus);
    pime_status audio_route(uint32_t route);
    pime_status permission(const std::string& capability, bool granted);

    pime_status plugin_load(const std::string& package_dir);
    pime_status plugin_unload(const std::string& id, const std::string& version);
    pime_status plugin_enable(const std::string& id, const std::string& version,
                              bool enabled);
    pime_status plugin_list(std::vector<uint8_t>* out);
    pime_status generation_build(const uint8_t* config, uint32_t len,
                                 uint64_t* generation);
    pime_status generation_activate(uint64_t generation);
    pime_status generation_retire(uint64_t generation);

    uint64_t active_generation() const { return active_generation_; }

    /* host::HostBridge */
    pime_status check(const host::HostCallContext& context,
                      const std::string& capability) override;
    pime_status invoke(const host::HostCallContext& context, host::HostKind kind,
                       uint8_t op, const uint8_t* in, uint32_t in_len,
                       std::vector<uint8_t>* out) override;
    pime_status emit(uint64_t session_id, const std::string& alias,
                     uint64_t generation, const uint8_t* data,
                     uint32_t len) override;

    void pump();

private:
    struct BuildRequest {
        std::string id;
        std::string version;
        std::string alias;
        std::string role;
        codec::Value settings = codec::Value::map_v();
    };

    pime_status run_event(Session* session, const codec::Value& event,
                          bool* handled, std::vector<codec::Value>* commands);
    pime_status ingest_output(Session* session, const ChainItem& item,
                              const codec::Value& output,
                              cap::Workspace* workspace, bool* handled,
                              std::vector<codec::Value>* commands);
    pime_status apply_commands(Session* session,
                               const std::vector<codec::Value>& commands);
    pime_status apply_command(Session* session, const codec::Value& command);
    pime_status deliver_async(const AsyncEvent& event);
    void enqueue_async(const std::string& alias, uint64_t generation,
                       codec::Value payload);
    void wake();
    void drain();
    Session* session_for_alias(const std::string& alias);

    phl::ContextCache* ctx_cache(uint64_t session_id);
    Generation* active_gen();
    ChainItem* chain_item(Generation* generation, const std::string& alias);
    void setup_host_calls();

    /* plugin / generation helpers */
    InstalledPlugin* find_installed(const std::string& id,
                                    const std::string& version);
    InstalledPlugin* resolve_installed(const std::string& id,
                                       const std::string& constraint);
    pime_status add_installed(const codec::Package& package);
    pime_status resolve_chain(const std::vector<BuildRequest>& requests,
                              Generation* generation);
    pime_status resolve_request(const BuildRequest& request,
                                const std::string& alias,
                                std::vector<BuildRequest>* explicit_requests,
                                std::vector<std::string>* visiting,
                                std::vector<std::string>* added,
                                Generation* generation);
    pime_status instantiate(const BuildRequest& request,
                            const InstalledPlugin& plugin,
                            Generation* generation);
    bool platform_matches(const codec::Manifest& manifest) const;
    pime_status build_plugin_config(const InstalledPlugin& plugin,
                                    const BuildRequest& request,
                                    codec::Value* out) const;
    codec::Value global_config() const;
    static bool parse_setting(const codec::SettingSpec& spec,
                              const codec::Value& value,
                              codec::Value* normalized);

    CoreOptions options_;
    pime_host_sink sink_{};
    void* sink_ud_ = nullptr;

    cap::Log log_;
    cap::Metrics metrics_;
    cap::Permission permission_;
    cap::AssetService assets_;
    std::unique_ptr<cap::Store> store_;
    cap::AudioStreams audio_;
    std::unique_ptr<cap::MlHost> ml_;
    std::unique_ptr<cap::NetService> net_;
    std::unique_ptr<host::PackageManager> packages_;
    cap::TimerService timers_;

    phl::BufferPool buffers_;
    std::map<uint64_t, std::unique_ptr<phl::ContextCache>> contexts_;
    phl::HandleRegistry<phl::Channel> channels_;
    phl::HandleRegistry<phl::Mutex> mutexes_;
    phl::HandleRegistry<phl::Condvar> condvars_;
    phl::HandleRegistry<phl::SharedBuffer> shared_buffers_;

    std::mutex thread_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<phl::PluginThread>> threads_;
    uint32_t next_thread_id_ = 1;

    std::map<uint64_t, std::unique_ptr<Session>> sessions_;
    uint64_t next_session_ = 1;
    std::map<std::string, uint64_t> last_session_by_alias_;

    std::vector<std::unique_ptr<InstalledPlugin>> installed_;
    std::map<uint64_t, std::unique_ptr<Generation>> generations_;
    uint64_t active_generation_ = 0;
    uint64_t next_generation_ = 1;

    std::mutex queue_mutex_;
    std::deque<AsyncEvent> async_queue_;
    std::deque<EmitEnvelope> emit_queue_;
    std::atomic<bool> wake_pending_{false};

    cap::Workspace* active_workspace_ = nullptr;
    bool paused_ = false;
    bool secure_mode_ = false;
};

}  // namespace pime::core

#endif
