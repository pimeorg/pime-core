/*
 * Native plugin ABI (trusted escape hatch). Same CBOR schema as WASM/Lua.
 */
#ifndef PIME_NATIVE_PLUGIN_H
#define PIME_NATIVE_PLUGIN_H

#include <pime/abi.h>

#if defined(_WIN32)
#define PIME_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PIME_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Host services exposed to native plugins. Same CBOR request/response shape
 * as the WASM/Lua host API. */
typedef struct pime_native_host {
    uint32_t size, version;
    void* ud;
    pime_status (*check)(void* ud, const char* capability, uint32_t len);
    pime_status (*invoke)(void* ud, uint32_t kind, uint32_t op,
                          const uint8_t* in, uint32_t in_len, uint8_t* out,
                          uint32_t cap, uint32_t* out_len);
    pime_status (*emit)(void* ud, uint64_t session_id, const uint8_t* data,
                        uint32_t len);
} pime_native_host;

typedef struct pime_native_plugin {
    uint32_t size, version;
    pime_status (*init)(const uint8_t* config, uint32_t len, void** state);
    pime_status (*process)(void* state, const uint8_t* in, uint32_t in_len,
                           uint8_t* out, uint32_t cap, uint32_t* out_len);
    pime_status (*on_event)(void* state, const uint8_t* evt, uint32_t len,
                            uint8_t* out, uint32_t cap, uint32_t* out_len);
    void (*shutdown)(void* state);
    /* Optional; appended in ABI 1.1. Must be called after init. */
    pime_status (*set_host)(void* state, const pime_native_host* host);
} pime_native_plugin;

PIME_PLUGIN_EXPORT pime_status pime_native_plugin_query(uint32_t abi_major,
                                                        pime_native_plugin* out);

#ifdef __cplusplus
}
#endif

#endif
