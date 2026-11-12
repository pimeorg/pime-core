#include "signature.hpp"

#include "util/base64.hpp"
#include "util/string_util.hpp"

namespace pime::codec {

pime_status parse_signature(std::string_view text, Signature* out) {
    if (!out) return PIME_INVALID;
    Signature sig;
    bool have_sig = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim_view(text.substr(start, end - start));
        if (!line.empty() && line.front() != '#') {
            size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string key(to_lower(trim_view(line.substr(0, colon))));
                std::string value(trim_view(line.substr(colon + 1)));
                if (key == "alg") sig.alg = value;
                else if (key == "key_id") sig.key_id = value;
                else if (key == "sig") {
                    have_sig = base64_decode(value, &sig.bytes);
                    if (!have_sig) return PIME_CORRUPT;
                }
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    if (!have_sig || sig.alg.empty() || sig.key_id.empty()) return PIME_CORRUPT;
    sig.valid = true;
    *out = std::move(sig);
    return PIME_OK;
}

std::string serialize_signature(const Signature& signature) {
    std::string out;
    out += "alg: " + signature.alg + "\n";
    out += "key_id: " + signature.key_id + "\n";
    out += "sig: " + base64_encode(signature.bytes.data(), signature.bytes.size()) + "\n";
    return out;
}

}  // namespace pime::codec
