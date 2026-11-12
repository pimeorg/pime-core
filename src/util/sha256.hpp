#ifndef PIME_UTIL_SHA256_HPP
#define PIME_UTIL_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pime::crypto_hash {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void update(std::string_view text) { update(text.data(), text.size()); }
    std::array<uint8_t, 32> final();

private:
    void transform(const uint8_t block[64]);
    uint32_t state_[8];
    uint64_t total_;
    uint8_t buffer_[64];
    size_t buffer_len_;
    bool finished_;
};

std::array<uint8_t, 32> sha256(const void* data, size_t len);
std::array<uint8_t, 32> sha256(std::string_view text);

std::string hex(const uint8_t* data, size_t len);
inline std::string hex(const std::array<uint8_t, 32>& d) {
    return hex(d.data(), d.size());
}

}  // namespace pime::crypto_hash

#endif
