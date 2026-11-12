#include <pime/core.h>

#include <cstdio>
#include <string>
#include <vector>

#include "codec/manifest.hpp"
#include "codec/package.hpp"
#include "host/package.hpp"
#include "platform/platform.hpp"
#include "util/result.hpp"
#include "util/semver.hpp"

namespace {

constexpr const char* kUsage =
    "usage: pimelint <package-dir> [--core <ver>] [--platform <name>]\n";

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += values[i];
    }
    return out;
}

bool has_file(const pime::codec::Package& package, const std::string& path) {
    for (const auto& file : package.files)
        if (file.path == path) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string dir = argv[1];
    std::string core = "1.0.0";
    std::string target_platform = pime::platform::current_platform();
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--core" && i + 1 < argc) {
            core = argv[++i];
        } else if (arg == "--platform" && i + 1 < argc) {
            target_platform = argv[++i];
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }

    pime::host::PackageConfig config;
    config.core_version = pime::parse_semver(core);
    config.strict_signature = false;
    config.allow_insecure = true;
    const pime::host::PackageManager manager(config);

    pime::codec::Package package;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    const pime_status status = manager.scan(dir, &package);
    if (status != PIME_OK) {
        std::printf("FAIL %s (%s)\n", dir.c_str(),
                    package.error.empty() ? pime::status_name(status)
                                          : package.error.c_str());
        return 1;
    }

    const pime::codec::Manifest& manifest = package.manifest;
    std::printf("package: %s %s (%s)\n", manifest.id.c_str(),
                manifest.version.c_str(), manifest.runtime.c_str());

    if (manifest.abi_major != PIME_ABI_MAJOR)
        errors.push_back("manifest abi major does not match core");
    if (manifest.min_core.empty())
        warnings.push_back("min_core is empty");

    bool platform_ok = manifest.platforms.empty();
    for (const auto& platform : manifest.platforms)
        if (platform == target_platform) platform_ok = true;
    if (!platform_ok)
        errors.push_back("package does not target " + target_platform);

    for (const auto& capability : manifest.capabilities)
        if (!pime::codec::is_known_capability(capability))
            errors.push_back("unknown capability " + capability);

    for (const auto& asset : manifest.assets)
        if (!has_file(package, "assets/" + asset))
            errors.push_back("missing asset " + asset);

    for (const auto& model : manifest.models) {
        const std::string in_models = "models/" + model.path;
        if (!has_file(package, model.path) && !has_file(package, in_models))
            errors.push_back("missing model " + model.path);
    }

    if (!package.has_signature)
        warnings.push_back("package is not signed");

    for (const auto& item : warnings) std::printf("warn: %s\n", item.c_str());
    for (const auto& item : errors) std::printf("error: %s\n", item.c_str());

    if (!errors.empty()) {
        std::printf("FAIL %zu error(s)\n", errors.size());
        return 1;
    }
    std::printf("OK capabilities: %s\n", join(manifest.capabilities).c_str());
    return 0;
}
