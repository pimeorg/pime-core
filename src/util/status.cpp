#include "result.hpp"

namespace pime {

const char* status_name(Status s) {
    switch (s) {
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

}  // namespace pime
