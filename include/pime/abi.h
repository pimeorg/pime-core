/*
 * pime-core public ABI: version, export macros, status codes, buffer handles.
 * C only. No C++, STL, exceptions or platform headers.
 */
#ifndef PIME_ABI_H
#define PIME_ABI_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#  if defined(PIME_SHARED)
#    if defined(PIME_CORE_BUILD)
#      define PIME_API __declspec(dllexport)
#    else
#      define PIME_API __declspec(dllimport)
#    endif
#  else
#    define PIME_API
#  endif
#else
#  define PIME_API __attribute__((visibility("default")))
#endif

#define PIME_ABI_MAJOR 1u
#define PIME_ABI_MINOR 0u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pime_status {
    PIME_OK = 0,
    PIME_INVALID = 1,
    PIME_NOT_FOUND = 2,
    PIME_STALE = 3,
    PIME_BUSY = 4,
    PIME_TIMEOUT = 5,
    PIME_FULL = 6,
    PIME_EMPTY = 7,
    PIME_CLOSED = 8,
    PIME_POISONED = 9,
    PIME_BUFFER_TOO_SMALL = 10,
    PIME_NO_MEMORY = 11,
    PIME_RESOURCE = 12,
    PIME_QUEUE_FULL = 13,
    PIME_UNSUPPORTED = 14,
    PIME_DENIED = 15,
    PIME_IO = 16,
    PIME_CORRUPT = 17
} pime_status;

/* Buffer/sync/context handles: (generation << 32) | index; 0 is invalid. */
typedef uint64_t pime_buffer;

#define PIME_HANDLE_MASK_INDEX 0xFFFFFFFFull
#define PIME_HANDLE_INDEX(h) ((uint32_t)((h) & PIME_HANDLE_MASK_INDEX))
#define PIME_HANDLE_GENERATION(h) ((uint32_t)((h) >> 32))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIME_ABI_H */
