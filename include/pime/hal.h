/*
 * HAL: host <-> core data structures and sink callbacks.
 */
#ifndef PIME_HAL_H
#define PIME_HAL_H

#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIME_LOG_ERROR = 0,
    PIME_LOG_WARN = 1,
    PIME_LOG_INFO = 2,
    PIME_LOG_DEBUG = 3,
    PIME_LOG_TRACE = 4
} pime_log_level;

/* modifiers */
#define PIME_MOD_CTRL (1u << 0)
#define PIME_MOD_ALT (1u << 1)
#define PIME_MOD_SHIFT (1u << 2)
#define PIME_MOD_META (1u << 3)
#define PIME_MOD_CAPS (1u << 4)
#define PIME_MOD_NUM (1u << 5)
#define PIME_MOD_FN (1u << 6)

/* key actions */
#define PIME_KEY_DOWN 0u
#define PIME_KEY_UP 1u
#define PIME_KEY_REPEAT 2u

/* touch phases */
#define PIME_TOUCH_DOWN 0u
#define PIME_TOUCH_MOVE 1u
#define PIME_TOUCH_UP 2u
#define PIME_TOUCH_CANCEL 3u

/* context field types */
#define PIME_FIELD_TEXT 0u
#define PIME_FIELD_PASSWORD 1u
#define PIME_FIELD_NUMBER 2u
#define PIME_FIELD_EMAIL 3u
#define PIME_FIELD_URL 4u
#define PIME_FIELD_PHONE 5u
#define PIME_FIELD_SEARCH 6u
#define PIME_FIELD_UNKNOWN 0xFFFFFFFFu

/* input types */
#define PIME_INPUT_KEYBOARD 0u
#define PIME_INPUT_HANDWRITING 1u
#define PIME_INPUT_VOICE 2u
#define PIME_INPUT_OTHER 0xFFFFFFFFu

/* segment types */
#define PIME_SEG_RAW 0u
#define PIME_SEG_SYLLABLE 1u
#define PIME_SEG_WORD 2u
#define PIME_SEG_PHRASE 3u
#define PIME_SEG_PUNCTUATION 4u
#define PIME_SEG_OTHER 0xFFFFFFFFu

/* audio formats */
#define PIME_AUDIO_S16LE 0u
#define PIME_AUDIO_F32LE 1u

/* host capabilities */
#define PIME_HOST_SURROUNDING (1u << 0)
#define PIME_HOST_SECURE (1u << 1)
#define PIME_HOST_AUDIO_CAPTURE (1u << 2)
#define PIME_HOST_AUDIO_PLAYBACK (1u << 3)
#define PIME_HOST_CANDIDATES (1u << 4)
#define PIME_HOST_TOUCH (1u << 5)

/* core flags */
#define PIME_FLAG_ALLOW_INSECURE (1u << 0)
#define PIME_FLAG_STRICT_SIGNATURE (1u << 1)

/* network states */
#define PIME_NET_OFFLINE 0u
#define PIME_NET_ONLINE 1u
#define PIME_NET_METERED 2u
#define PIME_NET_UNMETERED 3u

/* audio focus */
#define PIME_FOCUS_LOST 0u
#define PIME_FOCUS_GAINED 1u
#define PIME_FOCUS_DUCKED 2u

/* audio routes */
#define PIME_ROUTE_SPEAKER 0u
#define PIME_ROUTE_RECEIVER 1u
#define PIME_ROUTE_HEADSET 2u
#define PIME_ROUTE_BLUETOOTH 3u

/* memory levels */
#define PIME_MEMORY_NORMAL 0u
#define PIME_MEMORY_WARNING 1u
#define PIME_MEMORY_CRITICAL 2u

/* keycodes: 0 unknown, 1..0x10FFFF Unicode scalar, 0x01000000|code special */
#define PIME_KEY_UNKNOWN 0u
#define PIME_KEY_SPECIAL_BASE 0x01000000u
#define PIME_KEYCODE_BACKSPACE (PIME_KEY_SPECIAL_BASE | 1u)
#define PIME_KEYCODE_TAB (PIME_KEY_SPECIAL_BASE | 2u)
#define PIME_KEYCODE_ENTER (PIME_KEY_SPECIAL_BASE | 3u)
#define PIME_KEYCODE_ESCAPE (PIME_KEY_SPECIAL_BASE | 4u)
#define PIME_KEYCODE_LEFT (PIME_KEY_SPECIAL_BASE | 5u)
#define PIME_KEYCODE_RIGHT (PIME_KEY_SPECIAL_BASE | 6u)
#define PIME_KEYCODE_UP (PIME_KEY_SPECIAL_BASE | 7u)
#define PIME_KEYCODE_DOWN (PIME_KEY_SPECIAL_BASE | 8u)
#define PIME_KEYCODE_HOME (PIME_KEY_SPECIAL_BASE | 9u)
#define PIME_KEYCODE_END (PIME_KEY_SPECIAL_BASE | 10u)
#define PIME_KEYCODE_PAGE_UP (PIME_KEY_SPECIAL_BASE | 11u)
#define PIME_KEYCODE_PAGE_DOWN (PIME_KEY_SPECIAL_BASE | 12u)
#define PIME_KEYCODE_INSERT (PIME_KEY_SPECIAL_BASE | 13u)
#define PIME_KEYCODE_DELETE (PIME_KEY_SPECIAL_BASE | 14u)
#define PIME_KEYCODE_F1 (PIME_KEY_SPECIAL_BASE | 0x10u)
#define PIME_KEYCODE_F(n) (PIME_KEYCODE_F1 + (uint32_t)(n) - 1u)

typedef struct {
    uint32_t size, version;
    const char* plugin_dir; uint32_t plugin_dir_len;
    const char* data_dir; uint32_t data_dir_len;
    const char* backend_dir; uint32_t backend_dir_len;
    const char* trust_store; uint32_t trust_store_len;
    const char* proxy; uint32_t proxy_len;
    const char* ca_file; uint32_t ca_file_len;
    const char* client_cert; uint32_t client_cert_len;
    const char* client_key; uint32_t client_key_len;
    const uint8_t* store_key; uint32_t store_key_len;
    uint32_t log_level;
    uint32_t flags;
    uint64_t memory_budget_mb;
} pime_core_config;

typedef struct {
    uint32_t size, version;
    uint64_t id; /* 0 = core assigns */
    uint32_t flags;
} pime_session_config;

typedef struct {
    uint32_t size, version;
    uint64_t time_ms;
    uint32_t level;
    const char* source; uint32_t source_len;
    const char* tag; uint32_t tag_len;
    const char* msg; uint32_t msg_len;
} pime_log_record;

typedef struct {
    uint32_t size, version;
    const char* text; uint32_t text_len;
} pime_text;

typedef struct {
    uint32_t size, version;
    uint32_t keycode;
    uint32_t modifiers;
    uint32_t action;
    const char* text; uint32_t text_len;
    uint64_t time_ms;
} pime_key_event;

typedef struct {
    uint32_t x, y, pressure;
    uint64_t t_ms;
} pime_touch_point;

typedef struct {
    uint32_t size, version;
    uint32_t phase;
    const pime_touch_point* points; uint32_t point_count;
    uint64_t time_ms;
} pime_touch_event;

typedef struct {
    uint32_t size, version;
    uint32_t index;
} pime_candidate_event;

typedef struct {
    uint32_t size, version;
    const char* app_id; uint32_t app_id_len;
    uint32_t field_type;
    uint32_t input_type;
    const char* locale; uint32_t locale_len;
    uint32_t page_size;
    uint32_t host_caps;
} pime_context;

typedef struct {
    uint32_t start, end, type;
} pime_segment;

typedef struct {
    uint32_t size, version;
    const char* text; uint32_t text_len;
    uint32_t cursor;
    const pime_segment* segments; uint32_t segment_count;
} pime_composition;

typedef struct {
    const char* text; uint32_t text_len;
    const char* comment; uint32_t comment_len;
    const char* label; uint32_t label_len;
} pime_candidate;

typedef struct {
    uint32_t size, version;
    const pime_candidate* items; uint32_t count;
} pime_candidate_list;

typedef struct {
    uint32_t size, version;
    uint32_t rate, channels, format;
    const void* data; uint32_t data_len;
    uint64_t time_ms;
} pime_audio_frame;

/*
 * Core invokes the sink on the owner thread. A non-OK return means the host
 * rejected the command. Pointers are valid only for the duration of the call.
 */
typedef struct {
    uint32_t size, version;
    pime_status (*set_composition)(void* ud, const pime_composition*);
    pime_status (*commit)(void* ud, const pime_text*);
    pime_status (*show_candidates)(void* ud, const pime_candidate_list*);
    pime_status (*hide_candidates)(void* ud);
    pime_status (*audio_out)(void* ud, const pime_audio_frame*);
    pime_status (*beep)(void* ud);
    pime_status (*clear_ink)(void* ud);
    pime_status (*notify)(void* ud, pime_status code);
    void (*on_log)(void* ud, const pime_log_record*);
    void (*wake)(void* ud);
} pime_host_sink;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIME_HAL_H */
