#ifndef PIME_CODEC_PACKAGE_HPP
#define PIME_CODEC_PACKAGE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "manifest.hpp"
#include "signature.hpp"

namespace pime::codec {

struct PackageFile {
    std::string path;
    uint64_t size = 0;
    std::string sha256; /* hex */
};

struct Package {
    std::string dir;
    Manifest manifest;
    std::vector<PackageFile> files;
    std::string code_hash;
    Signature signature;
    bool has_signature = false;
    bool valid = false;
    std::string error;
};

/* Layout, manifest and hashes are assembled by the host layer, which owns
 * filesystem access. */

/* Canonical bytes covered by SIGNATURE. */
std::vector<uint8_t> package_signing_payload(const Package& package);

/* Hex SHA-256 over manifest + path/size/hash list. */
std::string compute_code_hash(const Package& package);

}  // namespace pime::codec

#endif
