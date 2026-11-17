/*
 * Core lifecycle, sessions, input and platform lifecycle.
 */
#ifndef PIME_CORE_H
#define PIME_CORE_H

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pime_core pime_core;
typedef struct pime_session pime_session;

PIME_API pime_core* pime_core_create(const pime_core_config* config,
                                     const pime_host_sink* sink, void* ud);
PIME_API void pime_core_destroy(pime_core* core);
PIME_API void pime_core_pump(pime_core* core);
PIME_API void pime_core_abi(uint32_t* major, uint32_t* minor);
PIME_API void pime_core_version(uint32_t* major, uint32_t* minor,
                                uint32_t* patch);
PIME_API const char* pime_status_string(pime_status status);
PIME_API void pime_log_set_level(pime_core* core, pime_log_level level);
PIME_API void pime_log_set_plugin_level(pime_core* core, const char* plugin_id,
                                        pime_log_level level);

/* sessions */
PIME_API pime_session* pime_session_create(pime_core* core,
                                           const pime_session_config* config);
PIME_API void pime_session_destroy(pime_session* session);
PIME_API uint64_t pime_session_id(const pime_session* session);

/* host -> core input (owner thread) */
PIME_API pime_status pime_core_key(pime_session* session,
                                   const pime_key_event* event, int* handled);
PIME_API pime_status pime_core_touch(pime_session* session,
                                     const pime_touch_event* event,
                                     int* handled);
PIME_API pime_status pime_core_text(pime_session* session,
                                    const pime_text* text);
PIME_API pime_status pime_core_candidate(pime_session* session,
                                         const pime_candidate_event* event);
PIME_API pime_status pime_core_surrounding(pime_session* session,
                                           const pime_text* before,
                                           const pime_text* after);
PIME_API pime_status pime_core_context(pime_session* session,
                                       const pime_context* context);
/* Updates the per-key candidate budget carried by frame.context without
 * dispatching an event. 0 restores the plugin's configured maximum. */
PIME_API pime_status pime_core_candidate_budget(pime_session* session,
                                                uint32_t budget);
PIME_API pime_status pime_core_focus(pime_session* session, int focused);
PIME_API pime_status pime_core_secure(pime_session* session, int secure);
PIME_API pime_status pime_core_reset(pime_session* session);
PIME_API pime_status pime_core_audio_in(pime_session* session,
                                        const pime_audio_frame* frame);

/* voice input control (owner thread). Dispatches a `voice` event to the
 * active chain; recognize plugins start/stop capture on it. */
#define PIME_VOICE_START 0u
#define PIME_VOICE_STOP 1u
#define PIME_VOICE_CANCEL 2u
PIME_API pime_status pime_core_voice(pime_session* session, uint32_t action);

/* platform lifecycle (owner thread) */
PIME_API pime_status pime_core_pause(pime_core* core);
PIME_API pime_status pime_core_resume(pime_core* core);
PIME_API pime_status pime_core_memory_warning(pime_core* core, uint32_t level);
PIME_API pime_status pime_core_network_changed(pime_core* core, uint32_t state);
PIME_API pime_status pime_core_audio_focus(pime_core* core, uint32_t focus);
PIME_API pime_status pime_core_audio_route(pime_core* core, uint32_t route);
PIME_API pime_status pime_core_permission(pime_core* core,
                                          const char* capability,
                                          uint32_t len, int granted);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIME_CORE_H */
