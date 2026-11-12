#ifndef PIME_CODEC_SIGNATURE_HPP
#define PIME_CODEC_SIGNATURE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pime::codec {

/* SIGNATURE is detached and text encoded:
 *   alg: ed25519
 *   key_id: pime-official-1
 *   sig: <base64>
 */
struct Signature {
    std::string alg;
    std::string key_id;
    std::vector<uint8_t> bytes;
    bool valid = false;
};

pime_status parse_signature(std::string_view text, Signature* out);
std::string serialize_signature(const Signature& signature);

}  // namespace pime::codec

#endif
