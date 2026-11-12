#include "utf8.hpp"

namespace pime::utf8 {

namespace {

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

bool decode(std::string_view in, size_t offset, char32_t* cp, size_t* next) {
    if (offset >= in.size()) return false;
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(in.data()) + offset;
    const size_t avail = in.size() - offset;
    const unsigned char c = p[0];

    if (c < 0x80) {
        *cp = c;
        *next = offset + 1;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && avail >= 2 && is_cont(p[1])) {
        char32_t v = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        if (v < 0x80) return false;
        *cp = v;
        *next = offset + 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && avail >= 3 && is_cont(p[1]) && is_cont(p[2])) {
        char32_t v = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return false;
        *cp = v;
        *next = offset + 3;
        return true;
    }
    if ((c & 0xF8) == 0xF0 && avail >= 4 && is_cont(p[1]) && is_cont(p[2]) &&
        is_cont(p[3])) {
        char32_t v = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                     ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return false;
        *cp = v;
        *next = offset + 4;
        return true;
    }
    return false;
}

size_t count(std::string_view in) {
    size_t n = 0;
    size_t i = 0;
    while (i < in.size()) {
        char32_t cp;
        size_t next;
        if (decode(in, i, &cp, &next)) {
            i = next;
        } else {
            ++i;
        }
        ++n;
    }
    return n;
}

size_t offset_of(std::string_view in, size_t cp_index) {
    size_t i = 0;
    size_t n = 0;
    while (i < in.size() && n < cp_index) {
        char32_t cp;
        size_t next;
        i = decode(in, i, &cp, &next) ? next : i + 1;
        ++n;
    }
    return i;
}

bool valid(std::string_view in) {
    size_t i = 0;
    while (i < in.size()) {
        char32_t cp;
        size_t next;
        if (!decode(in, i, &cp, &next)) return false;
        i = next;
    }
    return true;
}

void append(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string from_cp(char32_t cp) {
    std::string s;
    append(s, cp);
    return s;
}

}  // namespace pime::utf8
