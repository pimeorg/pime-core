#ifndef PIME_HOST_PACKAGE_HPP
#define PIME_HOST_PACKAGE_HPP

#include <pime/abi.h>

#include <string>
#include <vector>

#include "codec/manifest.hpp"
#include "codec/package.hpp"
#include "util/semver.hpp"

namespace pime::host {

struct PackageConfig {
    std::string plugin_dir;
    std::string trust_store;
    bool strict_signature = true;
    bool allow_insecure = false;
    SemVer core_version;
};

class PackageManager {
public:
    explicit PackageManager(PackageConfig config);

    pime_status scan(const std::string& dir, codec::Package* out) const;
    pime_status verify(const codec::Package& package) const;
    /* Verifies then atomically installs into plugin_dir/<id>/<version>. */
    pime_status install(const std::string& source_dir, codec::Package* out);
    pime_status uninstall(const std::string& id, const std::string& version) const;

    std::string install_path(const std::string& id,
                             const std::string& version) const;

    const PackageConfig& config() const { return config_; }

private:
    struct RootKey {
        std::string key_id;
        std::string pem;
    };

    std::vector<RootKey> load_roots() const;
    bool revoked(const std::string& key_id) const;

    PackageConfig config_;
};

}  // namespace pime::host

#endif
