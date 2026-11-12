#ifndef PIME_UTIL_STRING_UTIL_HPP
#define PIME_UTIL_STRING_UTIL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pime {

bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string_view trim_view(std::string_view s);
std::string to_lower(std::string_view s);
std::vector<std::string_view> split(std::string_view s, char sep);
bool parse_u32(std::string_view s, uint32_t* out);
bool parse_u64(std::string_view s, uint64_t* out);
bool parse_i64(std::string_view s, int64_t* out);

/* Replaces NUL and ASCII control characters with spaces. */
std::string sanitize_log_text(std::string_view s, size_t max_len);

/* True when path is relative, has no "." / ".." segments, and no drive. */
bool is_safe_relative_path(std::string_view path);

}  // namespace pime

#endif
