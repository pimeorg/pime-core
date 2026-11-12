#include "semver.hpp"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace pime {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

bool parse_num(std::string_view s, size_t* pos, uint32_t* out) {
    size_t start = *pos;
    uint64_t value = 0;
    while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos]))) {
        value = value * 10 + static_cast<uint64_t>(s[*pos] - '0');
        if (value > 0xFFFFFFFFull) return false;
        ++*pos;
    }
    if (*pos == start) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

int compare_prerelease(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;
    if (b.empty()) return -1;
    size_t ai = 0, bi = 0;
    while (ai < a.size() || bi < b.size()) {
        size_t ae = a.find('.', ai);
        size_t be = b.find('.', bi);
        if (ae == std::string_view::npos) ae = a.size();
        if (be == std::string_view::npos) be = b.size();
        std::string_view ap = a.substr(ai, ae - ai);
        std::string_view bp = b.substr(bi, be - bi);
        const bool an = !ap.empty() &&
                        ap.find_first_not_of("0123456789") == std::string_view::npos;
        const bool bn = !bp.empty() &&
                        bp.find_first_not_of("0123456789") == std::string_view::npos;
        int cmp;
        if (an && bn) {
            unsigned long long av = std::strtoull(std::string(ap).c_str(), nullptr, 10);
            unsigned long long bv = std::strtoull(std::string(bp).c_str(), nullptr, 10);
            cmp = av < bv ? -1 : (av > bv ? 1 : 0);
        } else if (an != bn) {
            cmp = an ? -1 : 1;
        } else {
            cmp = ap.compare(bp);
            cmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
        }
        if (cmp != 0) return cmp;
        if (ae == a.size() && be == b.size()) break;
        ai = ae + 1;
        bi = be + 1;
    }
    return 0;
}

struct Bound {
    uint32_t major, minor, patch;
};

bool wildcard_compare(const SemVer& v, const Bound& b, bool any_major,
                      bool any_minor, bool any_patch) {
    if (!any_major && v.major != b.major) return false;
    if (!any_minor && v.minor != b.minor) return false;
    if (!any_patch && v.patch != b.patch) return false;
    return true;
}

}  // namespace

SemVer parse_semver(std::string_view text) {
    SemVer out;
    text = trim(text);
    size_t pos = 0;
    if (!parse_num(text, &pos, &out.major)) return out;
    if (pos >= text.size() || text[pos] != '.') return out;
    ++pos;
    if (!parse_num(text, &pos, &out.minor)) return out;
    if (pos >= text.size() || text[pos] != '.') return out;
    ++pos;
    if (!parse_num(text, &pos, &out.patch)) return out;

    if (pos < text.size() && text[pos] == '-') {
        ++pos;
        size_t end = text.find('+', pos);
        out.prerelease = std::string(text.substr(pos, end - pos));
        pos = end;
        if (out.prerelease.empty()) return out;
    }
    if (pos < text.size() && text[pos] == '+') {
        ++pos;
        out.build = std::string(text.substr(pos));
        if (out.build.empty()) return out;
        pos = text.size();
    }
    if (pos != text.size()) return out;
    out.valid = true;
    return out;
}

int compare_semver(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return compare_prerelease(a.prerelease, b.prerelease);
}

bool version_satisfies(const SemVer& version, std::string_view constraint) {
    constraint = trim(constraint);
    if (constraint.empty() || constraint == "*" || constraint == "x") return true;
    if (!version.valid) return false;

    std::vector<std::string_view> parts;
    while (!constraint.empty()) {
        constraint = trim(constraint);
        if (constraint.empty()) break;
        size_t sp = constraint.find_first_of(" \t");
        parts.push_back(constraint.substr(0, sp));
        constraint = sp == std::string_view::npos ? std::string_view{}
                                                  : constraint.substr(sp + 1);
    }

    for (std::string_view part : parts) {
        if (part == "*") continue;
        enum class Op { Exact, Ge, Gt, Le, Lt, Caret, Tilde, Any } op = Op::Exact;
        if (part.rfind(">=", 0) == 0) { op = Op::Ge; part.remove_prefix(2); }
        else if (part.rfind("<=", 0) == 0) { op = Op::Le; part.remove_prefix(2); }
        else if (part.rfind(">", 0) == 0) { op = Op::Gt; part.remove_prefix(1); }
        else if (part.rfind("<", 0) == 0) { op = Op::Lt; part.remove_prefix(1); }
        else if (part.rfind("=", 0) == 0) { op = Op::Exact; part.remove_prefix(1); }
        else if (part.rfind("^", 0) == 0) { op = Op::Caret; part.remove_prefix(1); }
        else if (part.rfind("~", 0) == 0) { op = Op::Tilde; part.remove_prefix(1); }

        part = trim(part);
        if (part.empty() || part == "*" || part == "x" || part == "X") continue;

        Bound bound{};
        bool any_major = false, any_minor = false, any_patch = false;
        std::string s(part);
        std::vector<std::string> segs;
        size_t start = 0;
        while (true) {
            size_t dot = s.find('.', start);
            segs.push_back(s.substr(start, dot == std::string::npos ? std::string::npos
                                                                   : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        auto parse_seg = [](const std::string& seg, uint32_t* out, bool* any) {
            if (seg == "*" || seg == "x" || seg == "X") { *any = true; return true; }
            if (seg.empty()) return false;
            char* end = nullptr;
            unsigned long v = std::strtoul(seg.c_str(), &end, 10);
            if (!end || *end != '\0') return false;
            *out = static_cast<uint32_t>(v);
            return true;
        };
        if (segs.size() > 3) return false;
        if (!parse_seg(segs[0], &bound.major, &any_major)) return false;
        if (segs.size() > 1 && !parse_seg(segs[1], &bound.minor, &any_minor)) return false;
        else if (segs.size() == 1) any_minor = true;
        if (segs.size() > 2 && !parse_seg(segs[2], &bound.patch, &any_patch)) return false;
        else if (segs.size() < 3) any_patch = true;

        SemVer low = version;
        bool pass = false;
        switch (op) {
            case Op::Any:
                pass = true;
                break;
            case Op::Exact:
                pass = wildcard_compare(low, bound, any_major, any_minor, any_patch);
                break;
            case Op::Ge:
                pass = (low.major > bound.major) ||
                       (low.major == bound.major && low.minor > bound.minor) ||
                       (low.major == bound.major && low.minor == bound.minor &&
                        low.patch >= bound.patch);
                break;
            case Op::Gt:
                pass = (low.major > bound.major) ||
                       (low.major == bound.major && low.minor > bound.minor) ||
                       (low.major == bound.major && low.minor == bound.minor &&
                        low.patch > bound.patch);
                break;
            case Op::Le:
                pass = !((low.major > bound.major) ||
                         (low.major == bound.major && low.minor > bound.minor) ||
                         (low.major == bound.major && low.minor == bound.minor &&
                          low.patch > bound.patch));
                break;
            case Op::Lt:
                pass = !((low.major > bound.major) ||
                         (low.major == bound.major && low.minor > bound.minor) ||
                         (low.major == bound.major && low.minor == bound.minor &&
                          low.patch >= bound.patch));
                break;
            case Op::Caret: {
                bool ge = (low.major > bound.major) ||
                          (low.major == bound.major && low.minor > bound.minor) ||
                          (low.major == bound.major && low.minor == bound.minor &&
                           low.patch >= bound.patch);
                bool upper;
                if (bound.major > 0) {
                    upper = low.major == bound.major;
                } else if (bound.minor > 0) {
                    upper = low.major == 0 && low.minor == bound.minor;
                } else {
                    upper = low.major == 0 && low.minor == 0 && low.patch == bound.patch;
                }
                pass = ge && upper;
                break;
            }
            case Op::Tilde: {
                bool ge = (low.major > bound.major) ||
                          (low.major == bound.major && low.minor > bound.minor) ||
                          (low.major == bound.major && low.minor == bound.minor &&
                           low.patch >= bound.patch);
                bool upper = low.major == bound.major;
                if (!any_minor) upper = upper && low.minor == bound.minor;
                pass = ge && upper;
                break;
            }
        }
        if (!pass) return false;
    }
    return true;
}

}  // namespace pime
