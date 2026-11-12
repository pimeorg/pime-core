#include "manifest.hpp"

#include <algorithm>
#include <cctype>

#include "util/string_util.hpp"
#include "yaml.hpp"

namespace pime::codec {

namespace {

std::string text_or(const Value* v, std::string_view fallback = {}) {
    if (!v || v->type != Type::Text) return std::string(fallback);
    return v->text;
}

uint64_t uint_or(const Value* v, uint64_t fallback) {
    return v && v->is_number() ? v->as_uint() : fallback;
}

bool bool_or(const Value* v, bool fallback) {
    return v && v->type == Type::Bool ? v->boolean : fallback;
}

std::vector<std::string> string_list(const Value* v) {
    std::vector<std::string> out;
    if (!v || v->type != Type::Array) return out;
    for (const auto& item : v->array) {
        if (item.type == Type::Text) out.push_back(item.text);
    }
    return out;
}

std::vector<TensorSpec> tensor_list(const Value* v) {
    std::vector<TensorSpec> out;
    if (!v || v->type != Type::Array) return out;
    for (const auto& item : v->array) {
        if (item.type != Type::Map) continue;
        TensorSpec spec;
        spec.dtype = text_or(item.find("dtype"), "f32");
        const Value* shape = item.find("shape");
        if (shape && shape->type == Type::Array) {
            for (const auto& dim : shape->array) {
                if (dim.is_number()) spec.shape.push_back(static_cast<uint32_t>(dim.as_uint()));
            }
        }
        out.push_back(std::move(spec));
    }
    return out;
}

void parse_limits(const Value* v, ManifestLimits* out) {
    if (!v || v->type != Type::Map) return;
    out->memory_mb = static_cast<uint32_t>(uint_or(v->find("memory_mb"), out->memory_mb));
    out->instructions = uint_or(v->find("instructions"), out->instructions);
    out->time_ms = static_cast<uint32_t>(uint_or(v->find("time_ms"), out->time_ms));
    out->state_kb = static_cast<uint32_t>(uint_or(v->find("state_kb"), out->state_kb));
    out->workspace_kb = static_cast<uint32_t>(uint_or(v->find("workspace_kb"), out->workspace_kb));
    out->store_mb = static_cast<uint32_t>(uint_or(v->find("store_mb"), out->store_mb));
    out->candidates_max = static_cast<uint32_t>(uint_or(v->find("candidates_max"), out->candidates_max));
    out->scratch_bytes = static_cast<uint32_t>(uint_or(v->find("scratch_bytes"), out->scratch_bytes));
    out->max_frame_bytes = static_cast<uint32_t>(uint_or(v->find("max_frame_bytes"), out->max_frame_bytes));
    out->net_concurrency = static_cast<uint32_t>(uint_or(v->find("net_concurrency"), out->net_concurrency));
    out->net_streams = static_cast<uint32_t>(uint_or(v->find("net_streams"), out->net_streams));
    out->net_bytes = uint_or(v->find("net_bytes"), out->net_bytes);
    out->net_bandwidth_kbps = static_cast<uint32_t>(uint_or(v->find("net_bandwidth_kbps"), out->net_bandwidth_kbps));
    out->audio_frames = static_cast<uint32_t>(uint_or(v->find("audio_frames"), out->audio_frames));
    out->log_bytes_per_sec = static_cast<uint32_t>(uint_or(v->find("log_bytes_per_sec"), out->log_bytes_per_sec));
    out->log_record_bytes = static_cast<uint32_t>(uint_or(v->find("log_record_bytes"), out->log_record_bytes));
}

void parse_budget(const Value* v, ManifestBudget* out) {
    if (!v || v->type != Type::Map) return;
    out->sync_us = static_cast<uint32_t>(uint_or(v->find("sync_us"), out->sync_us));
    out->async_ms = static_cast<uint32_t>(uint_or(v->find("async_ms"), out->async_ms));
}

void parse_settings(const Value* v, std::vector<SettingSpec>* out) {
    if (!v || v->type != Type::Array) return;
    for (const auto& item : v->array) {
        if (item.type != Type::Map) continue;
        SettingSpec spec;
        spec.key = text_or(item.find("key"));
        spec.type = text_or(item.find("type"), "string");
        if (const Value* def = item.find("default")) {
            spec.default_value = *def;
            spec.has_default = true;
        }
        if (const Value* minv = item.find("min")) spec.min_value = minv->as_int();
        if (const Value* maxv = item.find("max")) spec.max_value = maxv->as_int();
        spec.values = string_list(item.find("values"));
        if (!spec.key.empty()) out->push_back(std::move(spec));
    }
}

void parse_models(const Value* v, std::vector<ModelSpec>* out) {
    if (!v || v->type != Type::Array) return;
    for (const auto& item : v->array) {
        if (item.type != Type::Map) continue;
        ModelSpec spec;
        spec.id = text_or(item.find("id"));
        spec.path = text_or(item.find("path"));
        spec.format = text_or(item.find("format"));
        spec.engine = text_or(item.find("engine"));
        spec.hash = text_or(item.find("hash"));
        spec.input = tensor_list(item.find("input"));
        spec.output = tensor_list(item.find("output"));
        if (!spec.id.empty()) out->push_back(std::move(spec));
    }
}

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 128) return false;
    if (!std::islower(static_cast<unsigned char>(id[0])) &&
        !std::isdigit(static_cast<unsigned char>(id[0])))
        return false;
    for (char c : id) {
        if (!std::islower(static_cast<unsigned char>(c)) &&
            !std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '_' &&
            c != '.')
            return false;
    }
    return true;
}

}  // namespace

bool is_known_role(std::string_view role) {
    static const char* kRoles[] = {"preprocess", "parse", "translate", "rank",
                                   "format", "recognize", "learn", "predict",
                                   "service"};
    for (const char* r : kRoles)
        if (role == r) return true;
    return false;
}

bool is_known_capability(std::string_view cap) {
    static const char* kCaps[] = {
        "input.read", "context.read", "surrounding.read", "output.emit",
        "config.read", "asset.read", "store.kv", "workspace", "thread", "emit",
        "ctx", "ctx.share", "channel", "sync", "shm", "time", "timer",
        "caps.query", "log", "metric", "net.http", "net.sse", "net.ws",
        "net.wss", "net.insecure", "random", "audio.pcm", "audio.opus",
        "audio.capture", "audio.playback", "ml.infer", "permission"};
    for (const char* c : kCaps)
        if (cap == c) return true;
    if (cap.rfind("crypto.", 0) == 0) return true;
    return false;
}

Manifest parse_manifest(std::string_view yaml_text) {
    Manifest m;
    Value root;
    std::string error;
    pime_status st = parse_yaml(yaml_text, &root, &error);
    if (st != PIME_OK || root.type != Type::Map) {
        m.error = error.empty() ? "manifest is not a mapping" : error;
        return m;
    }
    m.raw = root;

    m.id = text_or(root.find("id"));
    m.version = text_or(root.find("version"));
    m.min_core = text_or(root.find("min_core"));
    m.state_schema = static_cast<uint32_t>(uint_or(root.find("state_schema"), 0));
    m.store_schema = static_cast<uint32_t>(uint_or(root.find("store_schema"), 0));
    m.runtime = text_or(root.find("runtime"));
    m.role = text_or(root.find("role"));
    m.entry = text_or(root.find("entry"), "pime_process");
    m.trusted = bool_or(root.find("trusted"), false);

    if (const Value* abi = root.find("abi"); abi && abi->type == Type::Map) {
        m.abi_major = static_cast<uint32_t>(uint_or(abi->find("major"), 0));
        m.abi_minor = static_cast<uint32_t>(uint_or(abi->find("minor"), 0));
    }
    m.platforms = string_list(root.find("platforms"));
    m.capabilities = string_list(root.find("capabilities"));
    m.requirements = string_list(root.find("requires"));
    m.assets = string_list(root.find("assets"));

    parse_limits(root.find("limits"), &m.limits);
    parse_budget(root.find("budget"), &m.budget);
    parse_settings(root.find("settings"), &m.settings);
    parse_models(root.find("models"), &m.models);

    if (const Value* sig = root.find("signature"); sig && sig->type == Type::Map) {
        m.signature.alg = text_or(sig->find("alg"));
        m.signature.key_id = text_or(sig->find("key_id"));
    }
    m.valid = true;
    return m;
}

pime_status validate_manifest(const Manifest& m, const SemVer& core_version) {
    if (!m.valid) return PIME_INVALID;
    if (!valid_id(m.id)) return PIME_INVALID;
    SemVer version = parse_semver(m.version);
    if (!version.valid) return PIME_INVALID;
    if (m.abi_major != PIME_ABI_MAJOR || m.abi_minor > PIME_ABI_MINOR)
        return PIME_INVALID;
    SemVer min_core = parse_semver(m.min_core);
    if (!min_core.valid) return PIME_INVALID;
    if (compare_semver(min_core, core_version) > 0) return PIME_UNSUPPORTED;
    if (m.runtime != "wasm" && m.runtime != "lua") {
        if (!(m.runtime == "native" && m.trusted)) return PIME_INVALID;
    }
    if (!is_known_role(m.role)) return PIME_INVALID;
    if (m.entry.empty()) return PIME_INVALID;
    if (m.platforms.empty()) return PIME_INVALID;
    if (m.limits.state_kb == 0 || m.limits.state_kb > 1024) return PIME_INVALID;
    if (m.limits.workspace_kb == 0 || m.limits.workspace_kb > 4096) return PIME_INVALID;
    for (const auto& cap : m.capabilities) {
        if (!is_known_capability(cap)) return PIME_INVALID;
    }
    for (const auto& req : m.requirements) {
        size_t at = req.find('@');
        if (at == std::string::npos || at == 0 || at + 1 >= req.size())
            return PIME_INVALID;
    }
    for (const auto& asset : m.assets) {
        if (!is_safe_relative_path(asset)) return PIME_INVALID;
    }
    for (const auto& model : m.models) {
        if (model.id.empty() || !is_safe_relative_path(model.path)) return PIME_INVALID;
        if (model.hash.empty()) return PIME_INVALID;
    }
    return PIME_OK;
}

Value manifest_signing_form(const Manifest& manifest) {
    Value root = manifest.raw;
    if (root.type == Type::Map) {
        auto& pairs = root.map;
        pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                   [](const auto& kv) {
                                       return kv.first.type == Type::Text &&
                                              kv.first.text == "signature";
                                   }),
                    pairs.end());
    }
    return root;
}

}  // namespace pime::codec
