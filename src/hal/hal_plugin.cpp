#include "hal_internal.hpp"

#include <pime/plugin.h>

#include <cstring>

using namespace pime;

namespace {

pime_status read_ref(const pime_plugin_ref* ref, std::string* id,
                     std::string* version) {
    if (!ref) return PIME_INVALID;
    if (!hal::struct_ok(ref->size, ref->version, sizeof(pime_plugin_ref)))
        return PIME_INVALID;
    pime_status status = hal::copy_text(ref->id, ref->id_len, id);
    if (status != PIME_OK) return status;
    if (id->empty()) return PIME_INVALID;
    return hal::copy_text(ref->semver, ref->semver_len, version);
}

}  // namespace

extern "C" {

pime_status pime_plugin_load(pime_core* core, const char* package_dir,
                             uint32_t len) {
    if (!core || !core->impl) return PIME_INVALID;
    std::string path;
    pime_status status = hal::copy_text(package_dir, len, &path);
    if (status != PIME_OK) return status;
    if (path.empty()) return PIME_INVALID;
    return core->impl->plugin_load(path);
}

pime_status pime_plugin_unload(pime_core* core,
                               const pime_plugin_ref* ref) {
    if (!core || !core->impl) return PIME_INVALID;
    std::string id;
    std::string version;
    pime_status status = read_ref(ref, &id, &version);
    if (status != PIME_OK) return status;
    return core->impl->plugin_unload(id, version);
}

pime_status pime_plugin_enable(pime_core* core, const pime_plugin_ref* ref,
                               int enabled) {
    if (!core || !core->impl) return PIME_INVALID;
    std::string id;
    std::string version;
    pime_status status = read_ref(ref, &id, &version);
    if (status != PIME_OK) return status;
    return core->impl->plugin_enable(id, version, enabled != 0);
}

pime_status pime_plugin_list(pime_core* core, uint8_t* out, uint32_t cap,
                             uint32_t* out_len) {
    if (!core || !core->impl || !out_len) return PIME_INVALID;
    std::vector<uint8_t> encoded;
    pime_status status = core->impl->plugin_list(&encoded);
    if (status != PIME_OK) return status;
    *out_len = static_cast<uint32_t>(encoded.size());
    if (encoded.size() > cap) return PIME_BUFFER_TOO_SMALL;
    if (!encoded.empty()) {
        if (!out) return PIME_INVALID;
        std::memcpy(out, encoded.data(), encoded.size());
    }
    return PIME_OK;
}

pime_status pime_generation_build(pime_core* core, const uint8_t* config,
                                  uint32_t len, uint64_t* generation) {
    if (!core || !core->impl || !generation) return PIME_INVALID;
    if (len && !config) return PIME_INVALID;
    return core->impl->generation_build(config, len, generation);
}

pime_status pime_generation_activate(pime_core* core, uint64_t generation) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->generation_activate(generation);
}

pime_status pime_generation_retire(pime_core* core, uint64_t generation) {
    if (!core || !core->impl) return PIME_INVALID;
    return core->impl->generation_retire(generation);
}

}  // extern "C"
