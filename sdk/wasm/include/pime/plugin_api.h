/*
 * pime WASM plugin SDK. The core calls the exports below through WAMR;
 * all data crossing the boundary is definite-length CBOR (RFC 8949).
 *
 * The plugin must also export:
 *   uint32_t pime_scratch_ptr(void);   linear memory offset of scratch
 *   uint32_t pime_scratch_cap(void);   must be >= manifest.limits.scratch_bytes
 *
 * Buffer handles are uint64 (generation << 32) | index; 0 is invalid.
 */
#ifndef PIME_PLUGIN_API_H
#define PIME_PLUGIN_API_H

#include <pime/abi.h>

#if defined(__wasm__)
#define PIME_EXPORT __attribute__((visibility("default")))
#else
#define PIME_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* config = { settings: map, assets: map, abi: { major: uint, minor: uint } } */
PIME_EXPORT pime_status pime_init(const uint8_t* config, uint32_t len);

/* input  = { event, state, workspace, context, config, assets }
 * output = { state, workspace, handled, commands, generation } */
PIME_EXPORT pime_status pime_process(const uint8_t* in, uint32_t in_len,
                                     uint8_t* out, uint32_t cap,
                                     uint32_t* out_len);

PIME_EXPORT pime_status pime_on_event(const uint8_t* evt, uint32_t len,
                                      uint8_t* out, uint32_t cap,
                                      uint32_t* out_len);

PIME_EXPORT void pime_shutdown(void);

PIME_EXPORT uint32_t pime_scratch_ptr(void);
PIME_EXPORT uint32_t pime_scratch_cap(void);

/* ---- host functions, imported from the core ---- */

/* net */
uint32_t pime_host_net_request(const uint8_t* req, uint32_t len);
uint32_t pime_host_net_result(uint32_t id, uint8_t* out,
                                          uint32_t cap);
uint32_t pime_host_net_open(const uint8_t* proto,
                                        const uint8_t* url, uint32_t url_len);
pime_status pime_host_net_send(uint32_t id, const uint8_t* data,
                                           uint32_t len);
pime_status pime_host_net_close(uint32_t id);

/* crypto / audio */
pime_status pime_host_crypto(const uint8_t* req, uint8_t* out,
                                         uint32_t cap, uint32_t* out_len);
pime_status pime_host_audio(const uint8_t* req, uint8_t* out,
                                        uint32_t cap, uint32_t* out_len);

/* ml */
uint32_t pime_host_ml_load(const uint8_t* model_id, uint32_t len);
pime_status pime_host_ml_load_ex(const uint8_t* desc, uint32_t len,
                                             uint32_t* session);
pime_status pime_host_ml_run(uint32_t id, const uint8_t* in,
                                         uint32_t in_len, uint8_t* out,
                                         uint32_t cap, uint32_t* out_len);
uint32_t pime_host_ml_run_async(uint32_t id, const uint8_t* in,
                                            uint32_t in_len);
pime_status pime_host_ml_cancel(uint32_t job);
pime_status pime_host_ml_free(uint32_t id);

/* log */
pime_status pime_host_log(uint32_t level, const uint8_t* tag,
                                      uint32_t tag_len, const uint8_t* msg,
                                      uint32_t msg_len);

/* op-based namespaces: op is the first byte of the CBOR array
 * { op: uint, ... }; in/out are CBOR maps. */
pime_status pime_host_workspace(const uint8_t* op,
                                            const uint8_t* in, uint32_t in_len,
                                            uint8_t* out, uint32_t cap,
                                            uint32_t* out_len);
pime_status pime_host_store(const uint8_t* op, const uint8_t* in,
                                        uint32_t in_len, uint8_t* out,
                                        uint32_t cap, uint32_t* out_len);
pime_status pime_host_asset(const uint8_t* op, const uint8_t* in,
                                        uint32_t in_len, uint8_t* out,
                                        uint32_t cap, uint32_t* out_len);
pime_status pime_host_config(const uint8_t* op, const uint8_t* in,
                                         uint32_t in_len, uint8_t* out,
                                         uint32_t cap, uint32_t* out_len);
pime_status pime_host_ctx(const uint8_t* op, const uint8_t* in,
                                      uint32_t in_len, uint8_t* out,
                                      uint32_t cap, uint32_t* out_len);
pime_status pime_host_channel(const uint8_t* op, const uint8_t* in,
                                          uint32_t in_len, uint8_t* out,
                                          uint32_t cap, uint32_t* out_len);
pime_status pime_host_sync(const uint8_t* op, const uint8_t* in,
                                       uint32_t in_len, uint8_t* out,
                                       uint32_t cap, uint32_t* out_len);
pime_status pime_host_shm(const uint8_t* op, const uint8_t* in,
                                      uint32_t in_len, uint8_t* out,
                                      uint32_t cap, uint32_t* out_len);

/* time / random / timer / caps */
uint64_t pime_host_time(uint32_t kind);
pime_status pime_host_random(uint8_t* out, uint32_t len);
uint32_t pime_host_timer_start(uint32_t delay_ms, uint32_t repeat,
                                           const uint8_t* id, uint32_t len);
pime_status pime_host_timer_cancel(uint32_t id);
pime_status pime_host_caps(uint8_t* out, uint32_t cap,
                                       uint32_t* out_len);

/* threads */
pime_status pime_host_thread_create(const uint8_t* desc,
                                                uint32_t len, uint32_t* tid);
pime_status pime_host_thread_post(uint32_t tid,
                                              const uint8_t* msg,
                                              uint32_t len);
pime_status pime_host_thread_recv(uint32_t tid, uint8_t* out,
                                              uint32_t cap, uint32_t* out_len,
                                              uint32_t timeout_ms);
pime_status pime_host_thread_park(uint32_t tid,
                                              uint32_t timeout_ms);
pime_status pime_host_thread_unpark(uint32_t tid);
pime_status pime_host_thread_close(uint32_t tid);
pime_status pime_host_thread_join(uint32_t tid,
                                              uint32_t timeout_ms);
pime_status pime_host_thread_info(uint32_t tid, uint8_t* out,
                                              uint32_t cap, uint32_t* out_len);

/* async output to the owner pump */
pime_status pime_host_emit(uint64_t session_id, const uint8_t* out,
                                       uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* PIME_PLUGIN_API_H */
