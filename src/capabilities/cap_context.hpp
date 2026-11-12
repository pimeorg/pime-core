#ifndef PIME_CAPABILITIES_CAP_CONTEXT_HPP
#define PIME_CAPABILITIES_CAP_CONTEXT_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>

#include "codec/manifest.hpp"

namespace pime::cap {

/* Identity and limits of one plugin instance inside a generation. */
struct PluginCapContext {
    std::string alias;
    std::string plugin_id;
    std::string version;
    std::string code_hash;
    std::string package_dir;
    std::string asset_dir;
    std::string data_dir;
    uint64_t generation = 0;
    uint32_t state_schema = 0;
    uint32_t store_schema = 0;
    codec::ManifestLimits limits;
    const codec::Manifest* manifest = nullptr;

    std::string source() const { return "plugin:" + plugin_id + "@" + version; }
};

}  // namespace pime::cap

#endif
