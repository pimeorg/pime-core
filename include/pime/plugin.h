/*
 * Plugin installation and generation management.
 */
#ifndef PIME_PLUGIN_H
#define PIME_PLUGIN_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t size, version;
    const char* id; uint32_t id_len;
    const char* semver; uint32_t semver_len; /* empty = active version */
} pime_plugin_ref;

PIME_API pime_status pime_plugin_load(pime_core* core, const char* package_dir,
                                      uint32_t len);
PIME_API pime_status pime_plugin_unload(pime_core* core,
                                        const pime_plugin_ref* ref);
PIME_API pime_status pime_plugin_enable(pime_core* core,
                                        const pime_plugin_ref* ref, int enabled);
/* CBOR array: [{id, version, code_hash, active, enabled, generations, state}] */
PIME_API pime_status pime_plugin_list(pime_core* core, uint8_t* out,
                                      uint32_t cap, uint32_t* out_len);

PIME_API pime_status pime_generation_build(pime_core* core,
                                           const uint8_t* config, uint32_t len,
                                           uint64_t* generation);
PIME_API pime_status pime_generation_activate(pime_core* core,
                                              uint64_t generation);
PIME_API pime_status pime_generation_retire(pime_core* core,
                                            uint64_t generation);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIME_PLUGIN_H */
