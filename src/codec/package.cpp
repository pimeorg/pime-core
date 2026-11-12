#include "package.hpp"

#include <algorithm>

#include "util/sha256.hpp"
#include "yaml.hpp"

namespace pime::codec {

std::vector<uint8_t> package_signing_payload(const Package& package) {
    std::vector<uint8_t> payload;
    const std::string canonical =
        canonical_yaml(manifest_signing_form(package.manifest));
    payload.insert(payload.end(), canonical.begin(), canonical.end());
    payload.push_back(0);
    for (const auto& file : package.files) {
        if (file.path == "SIGNATURE" || file.path == "manifest.yaml") continue;
        payload.insert(payload.end(), file.path.begin(), file.path.end());
        payload.push_back(0);
        const std::string size = std::to_string(file.size);
        payload.insert(payload.end(), size.begin(), size.end());
        payload.push_back(0);
        payload.insert(payload.end(), file.sha256.begin(), file.sha256.end());
        payload.push_back(0);
    }
    return payload;
}

std::string compute_code_hash(const Package& package) {
    const std::vector<uint8_t> payload = package_signing_payload(package);
    return crypto_hash::hex(crypto_hash::sha256(payload.data(), payload.size()));
}

}  // namespace pime::codec
