#include "asset.hpp"

#include <cstring>

#include "platform/platform.hpp"
#include "util/string_util.hpp"

namespace pime::cap {

namespace {

pime_status collect_files(const std::string& root, const std::string& relative,
                          std::vector<std::string>* out, int depth) {
    if (depth > 8) return PIME_OK;
    const std::string dir = relative.empty() ? root : platform::path_join(root, relative);
    std::vector<platform::DirEntry> entries;
    pime_status st = platform::list_directory(dir, &entries);
    if (st != PIME_OK) return st;
    for (const auto& entry : entries) {
        const std::string child =
            relative.empty() ? entry.name : relative + "/" + entry.name;
        if (entry.is_dir) {
            collect_files(root, child, out, depth + 1);
        } else {
            out->push_back(child);
        }
    }
    return PIME_OK;
}

}  // namespace

pime_status AssetService::list(const std::string& root,
                               std::vector<std::string>* out) {
    if (!out) return PIME_INVALID;
    out->clear();
    return collect_files(root, "", out, 0);
}

pime_status AssetService::open(const std::string& root, const std::string& path,
                               uint64_t* handle) {
    if (!handle || !is_safe_relative_path(path)) return PIME_INVALID;
    const std::string full = platform::path_join(root, path);
    if (!platform::path_exists(full)) return PIME_NOT_FOUND;
    auto asset = std::make_shared<Asset>();
    asset->path = path;
    pime_status st = platform::read_file(full, &asset->data);
    if (st != PIME_OK) return st;
    *handle = assets_.insert(asset);
    return *handle ? PIME_OK : PIME_NO_MEMORY;
}

pime_status AssetService::size(uint64_t handle, uint64_t* size) const {
    if (!size) return PIME_INVALID;
    auto asset = assets_.get(handle, nullptr);
    if (!asset) return PIME_STALE;
    *size = asset->data.size();
    return PIME_OK;
}

pime_status AssetService::read(uint64_t handle, uint32_t offset, void* out,
                               uint32_t cap, uint32_t* out_len) const {
    if (!out_len) return PIME_INVALID;
    auto asset = assets_.get(handle, nullptr);
    if (!asset) return PIME_STALE;
    if (offset > asset->data.size()) return PIME_INVALID;
    const uint32_t avail = static_cast<uint32_t>(asset->data.size() - offset);
    *out_len = avail;
    if (avail > cap) return PIME_BUFFER_TOO_SMALL;
    if (avail && !out) return PIME_INVALID;
    if (avail) std::memcpy(out, asset->data.data() + offset, avail);
    return PIME_OK;
}

pime_status AssetService::map(uint64_t handle, const void** ptr,
                              uint32_t* size) const {
    if (!ptr || !size) return PIME_INVALID;
    auto asset = assets_.get(handle, nullptr);
    if (!asset) return PIME_STALE;
    *ptr = asset->data.data();
    *size = static_cast<uint32_t>(asset->data.size());
    return PIME_OK;
}

pime_status AssetService::close(uint64_t handle) { return assets_.erase(handle); }

}  // namespace pime::cap
