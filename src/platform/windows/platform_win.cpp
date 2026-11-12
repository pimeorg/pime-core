#include "platform.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>

namespace pime::platform {

int32_t timezone_offset_minutes() {
    TIME_ZONE_INFORMATION tz{};
    DWORD id = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias;
    if (id == TIME_ZONE_ID_DAYLIGHT) bias += tz.DaylightBias;
    else if (id == TIME_ZONE_ID_STANDARD) bias += tz.StandardBias;
    /* Windows bias is minutes west of UTC; return offset east of UTC. */
    return static_cast<int32_t>(-bias);
}

pime_status random_bytes(uint8_t* out, size_t len) {
    if (!out && len != 0) return PIME_INVALID;
    while (len > 0) {
        ULONG chunk = static_cast<ULONG>(len > 0x100000 ? 0x100000 : len);
        NTSTATUS st = BCryptGenRandom(nullptr, out, chunk,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (st != 0) return PIME_RESOURCE;
        out += chunk;
        len -= chunk;
    }
    return PIME_OK;
}

pime_status Library::open(const std::string& path, std::string* error) {
    close();
    HMODULE module = LoadLibraryA(path.c_str());
    if (!module) {
        if (error) *error = "LoadLibrary failed: " + std::to_string(GetLastError());
        return PIME_IO;
    }
    handle_ = reinterpret_cast<void*>(module);
    return PIME_OK;
}

void* Library::symbol(const char* name) {
    if (!handle_ || !name) return nullptr;
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
}

void Library::close() {
    if (handle_) {
        FreeLibrary(reinterpret_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
}

Library::~Library() { close(); }

}  // namespace pime::platform
