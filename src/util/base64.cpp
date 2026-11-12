#include "base64.hpp"

namespace pime {

namespace {

const char kEncode[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kEncode[(v >> 18) & 63]);
        out.push_back(kEncode[(v >> 12) & 63]);
        out.push_back(kEncode[(v >> 6) & 63]);
        out.push_back(kEncode[v & 63]);
        i += 3;
    }
    const size_t rest = len - i;
    if (rest == 1) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kEncode[(v >> 18) & 63]);
        out.push_back(kEncode[(v >> 12) & 63]);
        out += "==";
    } else if (rest == 2) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kEncode[(v >> 18) & 63]);
        out.push_back(kEncode[(v >> 12) & 63]);
        out.push_back(kEncode[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

bool base64_decode(std::string_view text, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();
    uint32_t accum = 0;
    int bits = 0;
    int pad = 0;
    for (char c : text) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') {
            ++pad;
            continue;
        }
        if (pad > 0) return false;
        int v = decode_char(c);
        if (v < 0) return false;
        accum = (accum << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    if (pad > 2) return false;
    if (bits >= 6) return false;
    return true;
}

}  // namespace pime
