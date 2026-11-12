#include "store.hpp"

#include <cstring>

#include "codec/cbor.hpp"
#include "crypto/crypto.hpp"
#include "platform/platform.hpp"

namespace pime::cap {

namespace {

constexpr uint32_t kStoreVersion = 1;
constexpr char kMagic[4] = {'P', 'M', 'S', 'T'};

void put_u32(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(static_cast<uint8_t>(v));
    out->push_back(static_cast<uint8_t>(v >> 8));
    out->push_back(static_cast<uint8_t>(v >> 16));
    out->push_back(static_cast<uint8_t>(v >> 24));
}

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string sanitize_alias(std::string_view alias) {
    std::string out;
    for (char c : alias) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out.empty() ? "_" : out;
}

}  // namespace

Store::Store(std::string data_dir, std::vector<uint8_t> key)
    : data_dir_(std::move(data_dir)), key_(std::move(key)) {}

std::string Store::file_for(const std::string& alias) const {
    return platform::path_join(platform::path_join(data_dir_, "store"),
                               sanitize_alias(alias) + ".bin");
}

Store::Namespace* Store::load_locked(const PluginCapContext& ctx) {
    auto it = namespaces_.find(ctx.alias);
    if (it != namespaces_.end() && it->second.loaded) return &it->second;
    Namespace& ns = namespaces_[ctx.alias];
    ns.loaded = true;
    ns.schema = ctx.store_schema;

    std::vector<uint8_t> file;
    if (platform::read_file(file_for(ctx.alias), &file) != PIME_OK) return &ns;
    constexpr size_t kHeader = 4 + 4 + 4 + 4 + 4 + 4 + 12 + 16;
    if (file.size() < kHeader) return &ns;
    if (std::memcmp(file.data(), kMagic, 4) != 0) return &ns;
    const uint32_t version = read_u32(file.data() + 4);
    if (version != kStoreVersion) return &ns;
    ns.schema = read_u32(file.data() + 8);
    const uint32_t nonce_len = read_u32(file.data() + 12);
    const uint32_t ct_len = read_u32(file.data() + 16);
    const uint32_t tag_len = read_u32(file.data() + 20);
    if (nonce_len != 12 || tag_len != 16 || kHeader + ct_len != file.size()) return &ns;

    const uint8_t* nonce = file.data() + 24;
    const uint8_t* tag = file.data() + 36;
    const uint8_t* ciphertext = file.data() + kHeader;
    std::vector<uint8_t> plaintext;
    pime_status st = crypto::aead_decrypt(
        "aes-256-gcm", key_, std::vector<uint8_t>(nonce, nonce + 12), {},
        ciphertext, ct_len, std::vector<uint8_t>(tag, tag + 16), &plaintext);
    if (st != PIME_OK) return &ns;

    codec::Value root;
    if (codec::decode(plaintext.data(), plaintext.size(), &root) != PIME_OK) return &ns;
    if (root.type != codec::Type::Map) return &ns;
    for (const auto& kv : root.map) {
        if (kv.first.type != codec::Type::Text) continue;
        if (kv.second.type == codec::Type::Text ||
            kv.second.type == codec::Type::Bytes) {
            ns.data[kv.first.text] = kv.second.text;
        }
    }
    return &ns;
}

pime_status Store::save_locked(const PluginCapContext& ctx, Namespace& ns) {
    if (key_.size() != 32) return PIME_DENIED;
    if (!crypto::have_openssl()) return PIME_UNSUPPORTED;
    codec::Value root = codec::Value::map_v();
    for (const auto& kv : ns.data) root.set(kv.first, codec::Value::bytes(kv.second));
    std::vector<uint8_t> plaintext;
    codec::encode(root, &plaintext);

    std::vector<uint8_t> nonce(12);
    pime_status st = crypto::random_bytes(nonce.data(), nonce.size());
    if (st != PIME_OK) return st;
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> tag;
    st = crypto::aead_encrypt("aes-256-gcm", key_, nonce, {}, plaintext.data(),
                              plaintext.size(), &ciphertext, &tag);
    if (st != PIME_OK) return st;

    std::vector<uint8_t> file;
    file.insert(file.end(), kMagic, kMagic + 4);
    put_u32(&file, kStoreVersion);
    put_u32(&file, ns.schema);
    put_u32(&file, static_cast<uint32_t>(nonce.size()));
    put_u32(&file, static_cast<uint32_t>(ciphertext.size()));
    put_u32(&file, static_cast<uint32_t>(tag.size()));
    file.insert(file.end(), nonce.begin(), nonce.end());
    file.insert(file.end(), tag.begin(), tag.end());
    file.insert(file.end(), ciphertext.begin(), ciphertext.end());

    const std::string dir = platform::path_join(data_dir_, "store");
    if (platform::create_directories(dir) != PIME_OK) return PIME_IO;
    const std::string path = file_for(ctx.alias);
    const std::string tmp = path + ".tmp";
    st = platform::write_file(tmp, file.data(), file.size());
    if (st != PIME_OK) return st;
    st = platform::replace_file(tmp, path);
    if (st == PIME_OK) ns.dirty = false;
    return st;
}

uint64_t Store::usage(const Namespace& ns) const {
    uint64_t total = 0;
    for (const auto& kv : ns.data) total += kv.first.size() + kv.second.size();
    return total;
}

pime_status Store::get(const PluginCapContext& ctx, const std::string& key,
                       std::string* value, bool* found) {
    if (!value || !found) return PIME_INVALID;
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    Namespace* ns = load_locked(ctx);
    auto it = ns->data.find(key);
    *found = it != ns->data.end();
    if (*found) *value = it->second;
    return PIME_OK;
}

pime_status Store::set(const PluginCapContext& ctx, const std::string& key,
                       std::string value) {
    if (key.empty() || key.size() > 1024) return PIME_INVALID;
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    Namespace* ns = load_locked(ctx);
    uint64_t limit = static_cast<uint64_t>(ctx.limits.store_mb) * 1024 * 1024;
    uint64_t after = usage(*ns);
    auto it = ns->data.find(key);
    if (it != ns->data.end()) after -= key.size() + it->second.size();
    after += key.size() + value.size();
    if (limit > 0 && after > limit) return PIME_FULL;
    ns->data[key] = std::move(value);
    ns->dirty = true;
    return PIME_OK;
}

pime_status Store::del(const PluginCapContext& ctx, const std::string& key,
                       bool* deleted) {
    if (!deleted) return PIME_INVALID;
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    Namespace* ns = load_locked(ctx);
    *deleted = ns->data.erase(key) != 0;
    if (*deleted) ns->dirty = true;
    return PIME_OK;
}

pime_status Store::batch(const PluginCapContext& ctx,
                         const std::vector<StoreOp>& ops) {
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    Namespace* ns = load_locked(ctx);
    std::map<std::string, std::string> next = ns->data;
    for (const auto& op : ops) {
        if (op.key.empty() || op.key.size() > 1024) return PIME_INVALID;
        if (op.kind == StoreOp::Kind::Set) next[op.key] = op.value;
        else next.erase(op.key);
    }
    uint64_t after = 0;
    for (const auto& kv : next) after += kv.first.size() + kv.second.size();
    uint64_t limit = static_cast<uint64_t>(ctx.limits.store_mb) * 1024 * 1024;
    if (limit > 0 && after > limit) return PIME_FULL;
    ns->data = std::move(next);
    ns->dirty = true;
    return PIME_OK;
}

pime_status Store::scan(const PluginCapContext& ctx, const std::string& prefix,
                        uint32_t limit,
                        std::vector<std::pair<std::string, std::string>>* out) {
    if (!out) return PIME_INVALID;
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    Namespace* ns = load_locked(ctx);
    out->clear();
    for (const auto& kv : ns->data) {
        if (!prefix.empty() && kv.first.rfind(prefix, 0) != 0) continue;
        out->emplace_back(kv.first, kv.second);
        if (limit != 0 && out->size() >= limit) break;
    }
    return PIME_OK;
}

pime_status Store::quota(const PluginCapContext& ctx, uint64_t* used,
                         uint64_t* limit) {
    if (!used || !limit) return PIME_INVALID;
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    Namespace* ns = load_locked(ctx);
    *used = usage(*ns);
    *limit = static_cast<uint64_t>(ctx.limits.store_mb) * 1024 * 1024;
    return PIME_OK;
}

pime_status Store::flush(const PluginCapContext& ctx) {
    if (key_.size() != 32) return PIME_DENIED;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = namespaces_.find(ctx.alias);
    if (it == namespaces_.end() || !it->second.dirty) return PIME_OK;
    return save_locked(ctx, it->second);
}

pime_status Store::flush_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pime_status result = PIME_OK;
    for (auto& kv : namespaces_) {
        if (!kv.second.dirty) continue;
        PluginCapContext dummy;
        dummy.alias = kv.first;
        dummy.store_schema = kv.second.schema;
        dummy.limits.store_mb = 16;
        pime_status st = save_locked(dummy, kv.second);
        if (st != PIME_OK) result = st;
    }
    return result;
}

}  // namespace pime::cap
