#ifndef PIME_CAPABILITIES_ASSET_HPP
#define PIME_CAPABILITIES_ASSET_HPP

#include <pime/abi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "phl/handle.hpp"

namespace pime::cap {

struct Asset {
    std::string path;
    std::vector<uint8_t> data;
};

/* Read-only assets under the plugin package's assets/ directory. */
class AssetService {
public:
    pime_status list(const std::string& root, std::vector<std::string>* out);
    pime_status open(const std::string& root, const std::string& path,
                     uint64_t* handle);
    pime_status size(uint64_t handle, uint64_t* size) const;
    pime_status read(uint64_t handle, uint32_t offset, uint32_t len, void* out,
                     uint32_t cap, uint32_t* out_len) const;
    pime_status map(uint64_t handle, const void** ptr, uint32_t* size) const;
    pime_status close(uint64_t handle);

private:
    phl::HandleRegistry<Asset> assets_;
};

}  // namespace pime::cap

#endif
