#include "string_util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace pime {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

std::string_view trim_view(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool parse_u32(std::string_view s, uint32_t* out) {
    if (s.empty()) return false;
    uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

bool parse_u64(std::string_view s, uint64_t* out) {
    if (s.empty()) return false;
    uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        if (value > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    *out = value;
    return true;
}

bool parse_i64(std::string_view s, int64_t* out) {
    bool neg = false;
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        neg = s.front() == '-';
        s.remove_prefix(1);
    }
    uint64_t v;
    if (!parse_u64(s, &v)) return false;
    if (neg) {
        if (v > static_cast<uint64_t>(INT64_MAX) + 1) return false;
        *out = v == static_cast<uint64_t>(INT64_MAX) + 1 ? INT64_MIN
                                                          : -static_cast<int64_t>(v);
    } else {
        if (v > static_cast<uint64_t>(INT64_MAX)) return false;
        *out = static_cast<int64_t>(v);
    }
    return true;
}

std::string sanitize_log_text(std::string_view s, size_t max_len) {
    std::string out;
    out.reserve(std::min(s.size(), max_len));
    for (char c : s) {
        if (out.size() >= max_len) break;
        unsigned char u = static_cast<unsigned char>(c);
        if (u == 0 || (u < 0x20 && u != '\t')) {
            out.push_back(' ');
        } else if (u == 0x7F) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool is_safe_relative_path(std::string_view path) {
    if (path.empty()) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    if (path.size() >= 2 && path[1] == ':') return false;
    for (std::string_view part : split(path, '/')) {
        if (part == ".." || part == "." || part.empty()) return false;
        if (part.find('\\') != std::string_view::npos) return false;
    }
    return path.find('\\') == std::string_view::npos;
}

}  // namespace pime
