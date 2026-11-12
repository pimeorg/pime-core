#ifndef PIME_UTIL_SEMVER_HPP
#define PIME_UTIL_SEMVER_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace pime {

struct SemVer {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    std::string prerelease;
    std::string build;
    bool valid = false;
};

SemVer parse_semver(std::string_view text);

/* Ignores build metadata. Prerelease sorts before release. */
int compare_semver(const SemVer& a, const SemVer& b);

/* True when version satisfies constraint. Supports *, exact, caret (^),
 * tilde (~), >=, >, <=, <, =, and space separated conjunctions. */
bool version_satisfies(const SemVer& version, std::string_view constraint);

}  // namespace pime

#endif
