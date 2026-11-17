#include "workspace.hpp"

#include <cstring>

namespace pime::cap {

Workspace::Workspace(uint32_t max_bytes) : max_bytes_(max_bytes) {}

void Workspace::reset() {
    values_.clear();
    staged_.clear();
    used_ = 0;
}

std::string Workspace::full_key(const std::string& alias, const std::string& key) {
    return "plugin/" + alias + "/" + key;
}

bool Workspace::valid_full_key(const std::string& key, std::string* alias) {
    const std::string prefix = "plugin/";
    if (key.rfind(prefix, 0) != 0) return false;
    size_t slash = key.find('/', prefix.size());
    if (slash == std::string::npos || slash == prefix.size()) return false;
    if (alias) *alias = key.substr(prefix.size(), slash - prefix.size());
    return true;
}

uint64_t Workspace::value_size(const std::string& key,
                               const codec::Value& value) const {
    uint64_t size = key.size() + 8;
    switch (value.type) {
        case codec::Type::Text:
        case codec::Type::Bytes: size += value.text.size(); break;
        case codec::Type::UInt:
        case codec::Type::Int:
        case codec::Type::Float: size += 8; break;
        case codec::Type::Bool:
        case codec::Type::Null: size += 1; break;
        default: size += 32; break;
    }
    return size;
}

pime_status Workspace::set(const std::string& alias, const std::string& key,
                           const codec::Value& value) {
    if (alias.empty() || key.empty()) return PIME_INVALID;
    const std::string full = full_key(alias, key);
    const uint64_t add = value_size(full, value);
    uint64_t existing = 0;
    auto old = values_.find(full);
    if (old != values_.end()) existing = value_size(full, old->second);
    auto old_staged = staged_.find(full);
    if (old_staged != staged_.end()) existing = value_size(full, old_staged->second);
    if (used_ + add - existing > max_bytes_) return PIME_FULL;
    used_ = used_ + add - existing;
    staged_[full] = value;
    return PIME_OK;
}

bool Workspace::get(const std::string& key, codec::Value* out) const {
    if (!out) return false;
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    *out = it->second;
    return true;
}

bool Workspace::get_self(const std::string& alias, const std::string& key,
                         codec::Value* out) const {
    return get(full_key(alias, key), out);
}

codec::Value Workspace::delta() const {
    codec::Value arr = codec::Value::array_v();
    for (const auto& kv : staged_) {
        codec::Value entry = codec::Value::map_v();
        entry.set("k", codec::Value::text_v(kv.first));
        entry.set("v", kv.second);
        arr.push(std::move(entry));
    }
    return arr;
}

pime_status Workspace::commit() {
    for (auto& kv : staged_) values_[kv.first] = std::move(kv.second);
    staged_.clear();
    return PIME_OK;
}

void Workspace::discard() { staged_.clear(); }

codec::Value Workspace::snapshot() const {
    codec::Value map = codec::Value::map_v();
    for (const auto& kv : values_) map.set(kv.first, kv.second);
    return map;
}

namespace {

void append_head(uint8_t major, uint64_t value, std::vector<uint8_t>* out) {
    const uint8_t base = static_cast<uint8_t>(major << 5);
    if (value < 24) {
        out->push_back(static_cast<uint8_t>(base | value));
    } else if (value <= 0xff) {
        out->push_back(static_cast<uint8_t>(base | 24));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out->push_back(static_cast<uint8_t>(base | 25));
        out->push_back(static_cast<uint8_t>(value >> 8));
        out->push_back(static_cast<uint8_t>(value));
    } else {
        out->push_back(static_cast<uint8_t>(base | 26));
        for (int shift = 24; shift >= 0; shift -= 8)
            out->push_back(static_cast<uint8_t>(value >> shift));
    }
}

}  // namespace

void Workspace::encode_snapshot(std::vector<uint8_t>* out) const {
    if (!out) return;
    out->clear();
    append_head(5, values_.size(), out);
    for (const auto& kv : values_) {
        codec::encode_append(codec::Value::text_v(kv.first), out);
        codec::encode_append(kv.second, out);
    }
}

}  // namespace pime::cap
