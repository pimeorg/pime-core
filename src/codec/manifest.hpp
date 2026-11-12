#ifndef PIME_CODEC_MANIFEST_HPP
#define PIME_CODEC_MANIFEST_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cbor.hpp"
#include "util/semver.hpp"

namespace pime::codec {

struct ManifestLimits {
    uint32_t memory_mb = 32;
    uint64_t instructions = 10000000ull;
    uint32_t time_ms = 5;
    uint32_t state_kb = 64;
    uint32_t workspace_kb = 256;
    uint32_t store_mb = 16;
    uint32_t candidates_max = 200;
    uint32_t scratch_bytes = 16384;
    uint32_t max_frame_bytes = 16384;
    uint32_t net_concurrency = 2;
    uint32_t net_streams = 2;
    uint64_t net_bytes = 1048576ull;
    uint32_t net_bandwidth_kbps = 256;
    uint32_t audio_frames = 64;
    uint32_t log_bytes_per_sec = 65536;
    uint32_t log_record_bytes = 1024;
};

struct ManifestBudget {
    uint32_t sync_us = 50;
    uint32_t async_ms = 20;
};

struct SignatureInfo {
    std::string alg;
    std::string key_id;
};

struct SettingSpec {
    std::string key;
    std::string type; /* bool / int / enum / string */
    Value default_value;
    bool has_default = false;
    int64_t min_value = 0;
    int64_t max_value = 0;
    std::vector<std::string> values;
};

struct TensorSpec {
    std::string dtype;
    std::vector<uint32_t> shape;
};

struct ModelSpec {
    std::string id;
    std::string path;
    std::string format;
    std::string engine;
    std::string hash;
    std::vector<TensorSpec> input;
    std::vector<TensorSpec> output;
};

struct Manifest {
    std::string id;
    std::string version;
    std::string min_core;
    std::string runtime; /* wasm / lua / native */
    std::string role;
    std::string entry;
    uint32_t abi_major = 0;
    uint32_t abi_minor = 0;
    uint32_t state_schema = 0;
    uint32_t store_schema = 0;
    bool trusted = false;

    std::vector<std::string> platforms;
    std::vector<std::string> capabilities;
    std::vector<std::string> requirements;
    std::vector<std::string> assets;

    ManifestLimits limits;
    ManifestBudget budget;
    SignatureInfo signature;
    std::vector<SettingSpec> settings;
    std::vector<ModelSpec> models;

    Value raw; /* parsed manifest including signature */

    bool valid = false;
    std::string error;
};

bool is_known_capability(std::string_view capability);
bool is_known_role(std::string_view role);

Manifest parse_manifest(std::string_view yaml_text);

/* core_version is the running core SemVer. */
pime_status validate_manifest(const Manifest& manifest,
                              const SemVer& core_version);

/* Raw manifest value without the signature section, for signing. */
Value manifest_signing_form(const Manifest& manifest);

}  // namespace pime::codec

#endif
