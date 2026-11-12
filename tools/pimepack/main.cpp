#include <pime/core.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capabilities/crypto/crypto.hpp"
#include "codec/manifest.hpp"
#include "codec/package.hpp"
#include "codec/signature.hpp"
#include "host/package.hpp"
#include "platform/platform.hpp"
#include "util/result.hpp"
#include "util/semver.hpp"
#include "util/string_util.hpp"

namespace {

constexpr const char* kUsage =
    "usage: pimepack <command> [options]\n"
    "  pack <dir> [-o <out>] [--core <ver>]\n"
    "  sign <dir> --key <private.pem> [--key-id <id>] [--core <ver>]\n"
    "  verify <dir> --root <roots.pem> [--core <ver>]\n"
    "  info <dir> [--core <ver>]\n";

int fail(const std::string& message) {
    std::fprintf(stderr, "pimepack: %s\n", message.c_str());
    return 2;
}

pime::host::PackageConfig make_config(const std::string& core) {
    pime::host::PackageConfig config;
    config.core_version = pime::parse_semver(core);
    config.strict_signature = false;
    config.allow_insecure = true;
    return config;
}

bool scan(const std::string& dir, const std::string& core,
          pime::codec::Package* out) {
    const pime::host::PackageManager manager(make_config(core));
    const pime_status status = manager.scan(dir, out);
    if (status != PIME_OK) {
        std::fprintf(stderr, "pimepack: %s\n",
                     out->error.empty() ? pime::status_name(status)
                                        : out->error.c_str());
        return false;
    }
    return true;
}

bool copy_package(const pime::codec::Package& package,
                  const std::string& target) {
    const std::string root = pime::platform::path_absolute(target);
    for (const auto& file : package.files) {
        const std::string source =
            pime::platform::path_join(package.dir, file.path);
        const std::string destination =
            pime::platform::path_join(root, file.path);
        const std::string parent = pime::platform::path_parent(destination);
        if (pime::platform::create_directories(parent) != PIME_OK)
            return false;
        if (pime::platform::copy_file(source, destination) != PIME_OK)
            return false;
    }
    return true;
}

bool read_key(const std::string& path, std::string* out) {
    return pime::platform::read_text_file(path, out) == PIME_OK;
}

int cmd_pack(const std::string& dir, const std::string& core,
             const std::string& output) {
    pime::codec::Package package;
    if (!scan(dir, core, &package)) return 1;

    const std::string name =
        "pime-plugin-" + package.manifest.id + "-" + package.manifest.version;
    const std::string target =
        output.empty()
            ? pime::platform::path_join(pime::platform::current_directory(), name)
            : output;
    if (!copy_package(package, target))
        return fail("failed to copy package files");
    std::printf("%s\ncode_hash: %s\n", target.c_str(), package.code_hash.c_str());
    return 0;
}

int cmd_sign(const std::string& dir, const std::string& core,
             const std::string& key_path, const std::string& key_id) {
    pime::codec::Package package;
    if (!scan(dir, core, &package)) return 1;
    if (!pime::cap::crypto::have_openssl())
        return fail("signing requires OpenSSL");

    std::string key;
    if (!read_key(key_path, &key)) return fail("cannot read private key");

    const std::vector<uint8_t> payload =
        pime::codec::package_signing_payload(package);
    pime::codec::Signature signature;
    signature.alg = "ed25519";
    signature.key_id = key_id;
    const pime_status status = pime::cap::crypto::sign_detached(
        key, signature.alg, payload.data(), payload.size(), &signature.bytes);
    if (status != PIME_OK)
        return fail(std::string("signing failed: ") + pime::status_name(status));

    const std::string text = pime::codec::serialize_signature(signature);
    const std::string path =
        pime::platform::path_join(dir, "SIGNATURE");
    if (pime::platform::write_file(path, text.data(), text.size()) != PIME_OK)
        return fail("failed to write SIGNATURE");
    std::printf("%s\n", path.c_str());
    return 0;
}

int cmd_verify(const std::string& dir, const std::string& core,
               const std::string& root) {
    pime::codec::Package package;
    if (!scan(dir, core, &package)) return 1;

    pime::host::PackageConfig config;
    config.core_version = pime::parse_semver(core);
    config.strict_signature = true;
    config.trust_store = root;
    const pime::host::PackageManager manager(config);
    const pime_status status = manager.verify(package);
    if (status != PIME_OK) {
        std::fprintf(stderr, "pimepack: verify failed: %s\n",
                     pime::status_name(status));
        return 1;
    }
    std::printf("signature ok (%s, key %s)\ncode_hash: %s\n",
                package.signature.alg.c_str(), package.signature.key_id.c_str(),
                package.code_hash.c_str());
    return 0;
}

int cmd_info(const std::string& dir, const std::string& core) {
    pime::codec::Package package;
    if (!scan(dir, core, &package)) return 1;
    std::printf("id: %s\nversion: %s\nruntime: %s\nrole: %s\nabi: %u.%u\n"
                "signed: %s\ncode_hash: %s\nfiles:\n",
                package.manifest.id.c_str(), package.manifest.version.c_str(),
                package.manifest.runtime.c_str(), package.manifest.role.c_str(),
                package.manifest.abi_major, package.manifest.abi_minor,
                package.has_signature ? "yes" : "no", package.code_hash.c_str());
    for (const auto& file : package.files)
        std::printf("  %s %llu %s\n", file.path.c_str(),
                    static_cast<unsigned long long>(file.size),
                    file.sha256.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string command = argv[1];
    const std::string dir = argv[2];
    std::string core = "1.0.0";
    std::string output;
    std::string key;
    std::string key_id = "pime-official-1";
    std::string root;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--core" && i + 1 < argc) {
            core = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            key = argv[++i];
        } else if (arg == "--key-id" && i + 1 < argc) {
            key_id = argv[++i];
        } else if (arg == "--root" && i + 1 < argc) {
            root = argv[++i];
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }

    if (command == "pack") return cmd_pack(dir, core, output);
    if (command == "sign") {
        if (key.empty()) return fail("--key is required");
        return cmd_sign(dir, core, key, key_id);
    }
    if (command == "verify") {
        if (root.empty()) return fail("--root is required");
        return cmd_verify(dir, core, root);
    }
    if (command == "info") return cmd_info(dir, core);
    std::fputs(kUsage, stderr);
    return 2;
}
