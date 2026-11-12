#ifndef PIME_HAL_INTERNAL_HPP
#define PIME_HAL_INTERNAL_HPP

#include <pime/abi.h>
#include <pime/core.h>
#include <pime/hal.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "core/core.hpp"

struct pime_core {
    pime::core::Core* impl = nullptr;
};

struct pime_session {
    pime::core::Core* core = nullptr;
    pime::core::Session* impl = nullptr;
};

namespace pime::hal {

inline bool struct_ok(uint32_t size, uint32_t version, size_t need) {
    return size >= need && version == PIME_ABI_MAJOR;
}

inline pime_status copy_text(const char* text, uint32_t len, std::string* out) {
    if (!out) return PIME_INVALID;
    if (len && !text) return PIME_INVALID;
    out->clear();
    if (!len) return PIME_OK;
    std::string_view view(text, len);
    if (view.find('\0') != std::string_view::npos) return PIME_INVALID;
    out->assign(view);
    return PIME_OK;
}

inline bool valid_text(const pime_text& text) {
    return struct_ok(text.size, text.version, sizeof(pime_text)) &&
           (text.text_len == 0 || text.text != nullptr);
}

inline bool valid_audio(const pime_audio_frame& frame) {
    return struct_ok(frame.size, frame.version, sizeof(pime_audio_frame)) &&
           (frame.data_len == 0 || frame.data != nullptr);
}

}  // namespace pime::hal

#endif
