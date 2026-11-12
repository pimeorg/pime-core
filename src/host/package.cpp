#include "package.hpp"

#include <algorithm>

#include "capabilities/crypto/crypto.hpp"
#include "platform/platform.hpp"
#include "util/sha256.hpp"
#include "util/string_util.hpp"

namespace pime::host {

namespace {

bool allowed_file(const std::string& path) {
    if (path == "manifest.yaml" || path == "SIGNATURE") return true;
    if (path == "plugin.wasm" || path == "plugin.lua") return true;
    if (path == "plugin.dll" || path == "plugin.so" || path == "plugin.dylib")
        return true;
    if (path.rfind("assets/", 0) == 0) return true;
    if (path.rfind("models/", 0) == 0) return true;
    return false;
}

pime_status collect_files(const std::string& root, const std::string& relative,
                          std::vector<codec::PackageFile>* files, int depth) {
    if (depth > 8) return PIME_INVALID;
    const std::string dir = relative.empty() ? root : platform::path_join(root, relative);
    std::vector<platform::DirEntry> entries;
    pime_status st = platform::list_directory(dir, &entries);
    if (st != PIME_OK) return st;
    for (const auto& entry : entries) {
        const std::string child =
            relative.empty() ? entry.name : relative + "/" + entry.name;
        if (!is_safe_relative_path(child)) return PIME_CORRUPT;
        if (entry.is_dir) {
            st = collect_files(root, child, files, depth + 1);
            if (st != PIME_OK) return st;
            continue;
        }
        if (!allowed_file(child)) return PIME_CORRUPT;
        std::vector<uint8_t> data;
        st = platform::read_file(platform::path_join(root, child), &data);
        if (st != PIME_OK) return st;
        codec::PackageFile file;
        file.path = child;
        file.size = data.size();
        file.sha256 = crypto_hash::hex(crypto_hash::sha256(data.data(), data.size()));
        files->push_back(std::move(file));
    }
    return PIME_OK;
}

std::string trim_line(std::string_view line) {
    return std::string(trim_view(line));
}

}  // namespace

PackageManager::PackageManager(PackageConfig config) : config_(std::move(config)) {}

std::string PackageManager::install_path(const std::string& id,
                                         const std::string& version) const {
    return platform::path_join(platform::path_join(config_.plugin_dir, id), version);
}

pime_status PackageManager::scan(const std::string& dir, codec::Package* out) const {
    if (!out || !platform::is_directory(dir)) return PIME_NOT_FOUND;
    codec::Package package;
    package.dir = platform::path_absolute(dir);

    std::string manifest_text;
    pime_status st =
        platform::read_text_file(platform::path_join(dir, "manifest.yaml"), &manifest_text);
    if (st != PIME_OK) return PIME_NOT_FOUND;
    package.manifest = codec::parse_manifest(manifest_text);
    if (!package.manifest.valid) {
        out->error = package.manifest.error;
        return PIME_CORRUPT;
    }
    st = codec::validate_manifest(package.manifest, config_.core_version);
    if (st != PIME_OK) {
        out->error = "manifest validation failed";
        return st;
    }
    st = collect_files(dir, "", &package.files, 0);
    if (st != PIME_OK) {
        out->error = "invalid package layout";
        return st;
    }
    std::sort(package.files.begin(), package.files.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });

    bool has_entry = false;
    for (const auto& file : package.files) {
        if (file.path == "plugin.wasm" || file.path == "plugin.lua" ||
            file.path == "plugin.dll" || file.path == "plugin.so" ||
            file.path == "plugin.dylib")
            has_entry = true;
    }
    if (!has_entry) {
        out->error = "plugin entry file missing";
        return PIME_CORRUPT;
    }

    std::string sig_text;
    if (platform::read_text_file(platform::path_join(dir, "SIGNATURE"), &sig_text) ==
        PIME_OK) {
        st = codec::parse_signature(sig_text, &package.signature);
        if (st != PIME_OK) return PIME_CORRUPT;
        package.has_signature = true;
        package.manifest.signature.alg = package.signature.alg;
        package.manifest.signature.key_id = package.signature.key_id;
    }
    package.code_hash = codec::compute_code_hash(package);
    package.valid = true;
    *out = std::move(package);
    return PIME_OK;
}

std::vector<PackageManager::RootKey> PackageManager::load_roots() const {
    std::vector<RootKey> keys;
    if (config_.trust_store.empty()) return keys;
    std::string text;
    if (platform::read_text_file(platform::path_join(config_.trust_store, "roots.pem"),
                                 &text) != PIME_OK)
        return keys;
    std::string current_id;
    std::string pem;
    bool in_key = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = trim_line(std::string_view(text).substr(start, end - start));
        if (line.rfind("# key_id:", 0) == 0) {
            current_id = trim_line(line.substr(9));
        } else if (line == "-----BEGIN PUBLIC KEY-----") {
            in_key = true;
            pem = line + "\n";
        } else if (in_key) {
            pem += line + "\n";
            if (line == "-----END PUBLIC KEY-----") {
                keys.push_back(RootKey{current_id, pem});
                current_id.clear();
                pem.clear();
                in_key = false;
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return keys;
}

bool PackageManager::revoked(const std::string& key_id) const {
    if (config_.trust_store.empty()) return false;
    std::string text;
    if (platform::read_text_file(platform::path_join(config_.trust_store, "revoked.txt"),
                                 &text) != PIME_OK)
        return false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = trim_line(std::string_view(text).substr(start, end - start));
        if (!line.empty() && line.front() != '#' && line == key_id) return true;
        if (end == text.size()) break;
        start = end + 1;
    }
    return false;
}

pime_status PackageManager::verify(const codec::Package& package) const {
    if (!package.valid) return PIME_CORRUPT;
    if (!config_.strict_signature) {
        return config_.allow_insecure ? PIME_OK : PIME_DENIED;
    }
    if (!package.has_signature) return PIME_DENIED;
    if (revoked(package.signature.key_id)) return PIME_DENIED;
    const std::vector<RootKey> keys = load_roots();
    if (keys.empty()) return PIME_DENIED;
    if (!cap::crypto::have_openssl()) return PIME_UNSUPPORTED;

    const std::vector<uint8_t> payload = codec::package_signing_payload(package);
    bool matched_key = false;
    for (const auto& key : keys) {
        if (!key.key_id.empty() && key.key_id != package.signature.key_id) continue;
        matched_key = true;
        pime_status st = cap::crypto::verify_signature(
            key.pem, package.signature.alg, payload.data(), payload.size(),
            package.signature.bytes.data(), package.signature.bytes.size());
        if (st == PIME_OK) return PIME_OK;
    }
    return matched_key ? PIME_DENIED : PIME_NOT_FOUND;
}

pime_status PackageManager::install(const std::string& source_dir,
                                    codec::Package* out) {
    if (!out) return PIME_INVALID;
    codec::Package package;
    pime_status st = scan(source_dir, &package);
    if (st != PIME_OK) {
        *out = std::move(package);
        return st;
    }
    st = verify(package);
    if (st != PIME_OK) return st;

    const std::string target = install_path(package.manifest.id, package.manifest.version);
    if (platform::is_directory(target)) {
        codec::Package existing;
        if (scan(target, &existing) == PIME_OK) {
            if (existing.code_hash == package.code_hash) {
                *out = std::move(package);
                return PIME_OK;
            }
        }
        return PIME_CORRUPT;
    }

    const std::string tmp = target + ".tmp";
    platform::remove_tree(tmp);
    st = platform::create_directories(tmp);
    if (st != PIME_OK) return PIME_IO;
    for (const auto& file : package.files) {
        std::vector<uint8_t> data;
        st = platform::read_file(platform::path_join(source_dir, file.path), &data);
        if (st != PIME_OK) {
            platform::remove_tree(tmp);
            return st;
        }
        const std::string dest = platform::path_join(tmp, file.path);
        const std::string parent = platform::path_parent(dest);
        if (platform::create_directories(parent) != PIME_OK) {
            platform::remove_tree(tmp);
            return PIME_IO;
        }
        st = platform::write_file(dest, data.data(), data.size());
        if (st != PIME_OK) {
            platform::remove_tree(tmp);
            return st;
        }
    }
    st = platform::replace_file(tmp, target);
    if (st != PIME_OK) {
        platform::remove_tree(tmp);
        return st;
    }
    *out = std::move(package);
    return PIME_OK;
}

pime_status PackageManager::uninstall(const std::string& id,
                                      const std::string& version) const {
    const std::string path = install_path(id, version);
    if (!platform::is_directory(path)) return PIME_NOT_FOUND;
    return platform::remove_tree(path);
}

}  // namespace pime::host
