#include "platform.hpp"

#include <dlfcn.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace pime::platform {

int32_t timezone_offset_minutes() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return static_cast<int32_t>(local.tm_gmtoff / 60);
}

pime_status random_bytes(uint8_t* out, size_t len) {
    if (!out && len != 0) return PIME_INVALID;
#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(out + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        done += static_cast<size_t>(n);
    }
    if (done == len) return PIME_OK;
#endif
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return PIME_RESOURCE;
    size_t n = std::fread(out, 1, len, f);
    std::fclose(f);
    return n == len ? PIME_OK : PIME_RESOURCE;
}

pime_status Library::open(const std::string& path, std::string* error) {
    close();
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (error) *error = dlerror() ? dlerror() : "dlopen failed";
        return PIME_IO;
    }
    handle_ = h;
    return PIME_OK;
}

void* Library::symbol(const char* name) {
    if (!handle_ || !name) return nullptr;
    return dlsym(handle_, name);
}

void Library::close() {
    if (handle_) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

Library::~Library() { close(); }

}  // namespace pime::platform
