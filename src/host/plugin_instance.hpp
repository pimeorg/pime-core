#ifndef PIME_HOST_PLUGIN_INSTANCE_HPP
#define PIME_HOST_PLUGIN_INSTANCE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codec/manifest.hpp"
#include "host_ops.hpp"
#include "phl/thread.hpp"

namespace pime::host {

/* One loaded plugin instance. Runs CBOR input/output frames. */
class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    virtual pime_status init(const std::vector<uint8_t>& config) = 0;
    virtual pime_status process(const uint8_t* in, uint32_t in_len,
                                std::vector<uint8_t>* out) = 0;
    virtual pime_status on_event(const uint8_t* evt, uint32_t evt_len,
                                 std::vector<uint8_t>* out) = 0;
    virtual void shutdown() = 0;
    virtual bool alive() const = 0;

    /* Updated before each call so host functions know the target session. */
    virtual void set_session(uint64_t session_id, bool secure,
                             uint32_t host_caps) {
        (void)session_id;
        (void)secure;
        (void)host_caps;
    }

    virtual phl::PluginThread::Entry thread_entry(const std::string& entry,
                                                  std::string* error) {
        (void)entry;
        (void)error;
        return {};
    }
};

/* Loads a plugin instance for the given runtime. Native is always available;
 * WASM/Lua require their build options and return UNSUPPORTED otherwise. */
pime_status create_instance(const codec::Manifest& manifest,
                            const std::string& package_dir,
                            const cap::PluginCapContext* caps,
                            HostBridge* bridge, const std::string& alias,
                            uint64_t generation,
                            std::unique_ptr<PluginInstance>* out);

}  // namespace pime::host

#endif
