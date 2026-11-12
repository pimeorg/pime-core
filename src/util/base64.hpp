#ifndef PIME_UTIL_BASE64_HPP
#define PIME_UTIL_BASE64_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pime {

std::string base64_encode(const uint8_t* data, size_t len);
bool base64_decode(std::string_view text, std::vector<uint8_t>* out);

}  // namespace pime

#endif
