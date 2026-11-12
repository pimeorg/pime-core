/*
 * Reference WASM plugin: uppercases key/text input, same behavior as the
 * native example. Build with a WASI toolchain, e.g.
 *
 *   clang++ --target=wasm32 -O2 -nostdlib -Wl,--no-entry \
 *           -Wl,--export=pime_init -Wl,--export=pime_process \
 *           -Wl,--export=pime_on_event -Wl,--export=pime_shutdown \
 *           -Wl,--export=pime_scratch_ptr -Wl,--export=pime_scratch_cap \
 *           -o plugin.wasm plugin.cpp
 *
 * The plugin avoids the C/C++ runtime; only the scratch buffer needs memory.
 */
#include <pime/plugin_api.h>

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kScratchBytes = 16384;
alignas(8) uint8_t g_scratch[kScratchBytes];
uint64_t g_commits = 0;

/* --- minimal CBOR writer ------------------------------------------------ */

uint8_t* put_head(uint8_t* out, uint8_t major, uint32_t value) {
    if (value < 24) {
        *out++ = static_cast<uint8_t>((major << 5) | value);
    } else if (value <= 0xff) {
        *out++ = static_cast<uint8_t>((major << 5) | 24);
        *out++ = static_cast<uint8_t>(value);
    } else {
        *out++ = static_cast<uint8_t>((major << 5) | 25);
        *out++ = static_cast<uint8_t>(value >> 8);
        *out++ = static_cast<uint8_t>(value);
    }
    return out;
}

uint8_t* put_text(uint8_t* out, const char* text, uint32_t length) {
    out = put_head(out, 3, length);
    for (uint32_t i = 0; i < length; ++i) *out++ = static_cast<uint8_t>(text[i]);
    return out;
}

/* Finds "text":"..." inside the decoded frame without a full CBOR parser.
 * The input frames produced by the core put event.text after "text". */
bool find_event_text(const uint8_t* in, uint32_t in_len, char* out,
                     uint32_t cap, uint32_t* out_len) {
    static const char kKey[] = "text";
    for (uint32_t i = 0; i + 4 < in_len; ++i) {
        if (in[i] != kKey[0] || in[i + 1] != kKey[1] || in[i + 2] != kKey[2] ||
            in[i + 3] != kKey[3])
            continue;
        /* text header follows the 4-byte key */
        const uint8_t head = in[i + 4];
        if (head >> 5 != 3) continue;
        uint32_t length = head & 0x1f;
        uint32_t offset = i + 5;
        if ((head & 0x1f) == 24) {
            if (offset >= in_len) continue;
            length = in[offset++];
        } else if ((head & 0x1f) >= 25) {
            continue;
        }
        if (offset + length > in_len || length > cap) continue;
        for (uint32_t j = 0; j < length; ++j)
            out[j] = static_cast<char>(in[offset + j]);
        *out_len = length;
        return true;
    }
    return false;
}

uint32_t write_response(uint8_t* out, uint32_t cap, bool handled,
                        const char* text, uint32_t text_len) {
    uint8_t buffer[512];
    uint8_t* p = buffer;
    p = put_head(p, 5, 3); /* { handled, state, commands? } */
    p = put_text(p, "handled", 7);
    *p++ = handled ? 0xf5 : 0xf4;
    p = put_text(p, "state", 5);
    p = put_head(p, 5, 1);
    p = put_text(p, "commits", 7);
    p = put_head(p, 0, static_cast<uint32_t>(g_commits));
    if (handled) {
        p = put_text(p, "commands", 8);
        p = put_head(p, 4, 1);
        p = put_head(p, 5, 2);
        p = put_text(p, "type", 4);
        p = put_text(p, "commit", 6);
        p = put_text(p, "text", 4);
        p = put_text(p, text, text_len);
    }
    const uint32_t size = static_cast<uint32_t>(p - buffer);
    if (size > cap) return size;
    for (uint32_t i = 0; i < size; ++i) out[i] = buffer[i];
    return size;
}

}  // namespace

extern "C" {

PIME_EXPORT pime_status pime_init(const uint8_t*, uint32_t) { return PIME_OK; }

PIME_EXPORT pime_status pime_process(const uint8_t* in, uint32_t in_len,
                                     uint8_t* out, uint32_t cap,
                                     uint32_t* out_len) {
    char text[256];
    uint32_t text_len = 0;
    bool handled = false;
    if (find_event_text(in, in_len, text, sizeof(text), &text_len)) {
        for (uint32_t i = 0; i < text_len; ++i)
            if (text[i] >= 'a' && text[i] <= 'z')
                text[i] = static_cast<char>(text[i] - 'a' + 'A');
        ++g_commits;
        handled = true;
    }
    const uint32_t size = write_response(out, cap, handled, text, text_len);
    *out_len = size;
    return size > cap ? PIME_BUFFER_TOO_SMALL : PIME_OK;
}

PIME_EXPORT pime_status pime_on_event(const uint8_t*, uint32_t, uint8_t* out,
                                      uint32_t cap, uint32_t* out_len) {
    const uint32_t size = write_response(out, cap, false, nullptr, 0);
    *out_len = size;
    return size > cap ? PIME_BUFFER_TOO_SMALL : PIME_OK;
}

PIME_EXPORT void pime_shutdown(void) {}

PIME_EXPORT uint32_t pime_scratch_ptr(void) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_scratch));
}

PIME_EXPORT uint32_t pime_scratch_cap(void) { return kScratchBytes; }

}  // extern "C"
