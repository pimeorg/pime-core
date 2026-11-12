#ifndef PIME_UTIL_UTF8_HPP
#define PIME_UTIL_UTF8_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pime::utf8 {

constexpr char32_t kReplacement = 0xFFFD;

/* Decodes one scalar value. Returns false on malformed input. */
bool decode(std::string_view in, size_t offset, char32_t* cp, size_t* next);

/* Number of Unicode scalar values (invalid bytes count as one each). */
size_t count(std::string_view in);

/* Byte offset for a scalar index; clamps to size(). */
size_t offset_of(std::string_view in, size_t cp_index);

bool valid(std::string_view in);

void append(std::string& out, char32_t cp);

std::string from_cp(char32_t cp);

}  // namespace pime::utf8

#endif
