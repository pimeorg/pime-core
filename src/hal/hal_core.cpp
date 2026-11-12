#include "hal_internal.hpp"

#include <pime/version.h>

#include <new>

using namespace pime;

namespace {

bool copy_pair(const char* text, uint32_t len, std::string* out) {
    return hal::copy_text(text, len, out) == PIME_OK;
}

}  // namespace

extern "C" {

pime_core* pime_core_create(const pime_core_config* config,
                            const pime_host_sink* sink, void* ud) {
    if (!config) return nullptr;
    if (!hal::struct_ok(config->size, config->version, sizeof(pime_core_config)))
        return nullptr;

    core::CoreOptions options;
    if (!copy_pair(config->plugin_dir, config->plugin_dir_len,
                   &options.plugin_dir) ||
        !copy_pair(config->data_dir, config->data_dir_len, &options.data_dir) ||
        !copy_pair(config->backend_dir, config->backend_dir_len,
                   &options.backend_dir) ||
        !copy_pair(config->trust_store, config->trust_store_len,
                   &options.trust_store) ||
        !copy_pair(config->proxy, config->proxy_len, &options.proxy) ||
        !copy_pair(config->ca_file, config->ca_file_len, &options.ca_file) ||
        !copy_pair(config->client_cert, config->client_cert_len,
                   &options.client_cert) ||
        !copy_pair(config->client_key, config->client_key_len,
                   &options.client_key))
        return nullptr;
    if (config->log_level > PIME_LOG_TRACE) return nullptr;
    if (config->store_key_len) {
        if (!config->store_key || config->store_key_len != 32) return nullptr;
        options.store_key.assign(config->store_key,
                                 config->store_key + config->store_key_len);
    }
    options.log_level = config->log_level;
    options.flags = config->flags;
    options.memory_budget_mb = config->memory_budget_mb;
    options.sink_ud = ud;

    if (sink) {
        if (!hal::struct_ok(sink->size, sink->version, sizeof(pime_host_sink)))
            return nullptr;
        options.sink = *sink;
    }

    pime_core* core = new (std::nothrow) pime_core();
    if (!core) return nullptr;
    core->impl = new (std::nothrow) core::Core(options);
    if (!core->impl) {
        delete core;
        return nullptr;
    }
    pime_status status = core->impl->initialize();
    if (status != PIME_OK) {
        delete core->impl;
        delete core;
        return nullptr;
    }
    return core;
}

void pime_core_destroy(pime_core* core) {
    if (!core) return;
    delete core->impl;
    delete core;
}

void pime_core_pump(pime_core* core) {
    if (!core || !core->impl) return;
    core->impl->pump();
}

void pime_core_abi(uint32_t* major, uint32_t* minor) {
    if (major) *major = PIME_ABI_MAJOR;
    if (minor) *minor = PIME_ABI_MINOR;
}

void pime_core_version(uint32_t* major, uint32_t* minor, uint32_t* patch) {
    if (major) *major = PIME_VERSION_MAJOR;
    if (minor) *minor = PIME_VERSION_MINOR;
    if (patch) *patch = PIME_VERSION_PATCH;
}

const char* pime_status_string(pime_status status) {
    switch (status) {
        case PIME_OK: return "OK";
        case PIME_INVALID: return "INVALID";
        case PIME_NOT_FOUND: return "NOT_FOUND";
        case PIME_STALE: return "STALE";
        case PIME_BUSY: return "BUSY";
        case PIME_TIMEOUT: return "TIMEOUT";
        case PIME_FULL: return "FULL";
        case PIME_EMPTY: return "EMPTY";
        case PIME_CLOSED: return "CLOSED";
        case PIME_POISONED: return "POISONED";
        case PIME_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
        case PIME_NO_MEMORY: return "NO_MEMORY";
        case PIME_RESOURCE: return "RESOURCE";
        case PIME_QUEUE_FULL: return "QUEUE_FULL";
        case PIME_UNSUPPORTED: return "UNSUPPORTED";
        case PIME_DENIED: return "DENIED";
        case PIME_IO: return "IO";
        case PIME_CORRUPT: return "CORRUPT";
    }
    return "UNKNOWN";
}

void pime_log_set_level(pime_core* core, pime_log_level level) {
    if (!core || !core->impl) return;
    if (level < PIME_LOG_ERROR || level > PIME_LOG_TRACE) return;
    core->impl->set_log_level(level);
}

void pime_log_set_plugin_level(pime_core* core, const char* plugin_id,
                               pime_log_level level) {
    if (!core || !core->impl || !plugin_id) return;
    if (level < PIME_LOG_ERROR || level > PIME_LOG_TRACE) return;
    const std::string id(plugin_id);
    if (id.empty() || id.size() > 256) return;
    core->impl->set_plugin_log_level(id, level);
}

pime_status pime_core_pause(pime_core* core) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->pause();
}

pime_status pime_core_resume(pime_core* core) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->resume();
}

pime_status pime_core_memory_warning(pime_core* core, uint32_t level) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->memory_warning(level);
}

pime_status pime_core_network_changed(pime_core* core, uint32_t state) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->network_changed(state);
}

pime_status pime_core_audio_focus(pime_core* core, uint32_t focus) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->audio_focus(focus);
}

pime_status pime_core_audio_route(pime_core* core, uint32_t route) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->audio_route(route);
}

pime_status pime_core_permission(pime_core* core, const char* capability,
                                 uint32_t len, int granted) {
    if (!core || !core->impl) return PIME_INVALID;
    std::string name;
    pime_status status = hal::copy_text(capability, len, &name);
    if (status != PIME_OK) return status;
    if (name.empty()) return PIME_INVALID;
    return core->impl->permission(name, granted != 0);
}

}  // extern "C"
