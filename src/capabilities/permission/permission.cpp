#include "permission.hpp"

#include "codec/manifest.hpp"
#include "util/string_util.hpp"

namespace pime::cap {

namespace {

std::string base_capability(const std::string& capability) {
    size_t paren = capability.find('(');
    return paren == std::string::npos ? capability : capability.substr(0, paren);
}

bool declared_cap(const std::set<std::string>& declared, const std::string& cap) {
    if (declared.count(cap)) return true;
    if (cap.rfind("crypto.", 0) == 0) return declared.count("crypto.*") != 0;
    return false;
}

}  // namespace

Permission::Permission(bool allow_insecure) : allow_insecure_(allow_insecure) {}

void Permission::declare(const std::string& alias,
                         const std::vector<std::string>& caps) {
    std::lock_guard<std::mutex> lock(mutex_);
    declared_[alias] = std::set<std::string>(caps.begin(), caps.end());
}

void Permission::undeclare(const std::string& alias) {
    std::lock_guard<std::mutex> lock(mutex_);
    declared_.erase(alias);
    per_plugin_.erase(alias);
}

pime_status Permission::grant(const std::string& spec, bool granted) {
    if (spec.empty()) return PIME_INVALID;
    std::string plugin;
    std::string capability = spec;
    size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        plugin = spec.substr(0, colon);
        capability = spec.substr(colon + 1);
        if (plugin.empty() || capability.empty()) return PIME_INVALID;
    }
    const bool has_detail = capability.find('(') != std::string::npos;
    std::string base = base_capability(capability);
    if (!codec::is_known_capability(base)) return PIME_INVALID;

    std::lock_guard<std::mutex> lock(mutex_);
    if (plugin.empty()) {
        global_[base] = granted;
        if (has_detail) global_[base + ".app"] = granted;
    } else {
        per_plugin_[plugin][base] = granted;
        if (has_detail) per_plugin_[plugin][base + ".app"] = granted;
    }
    return PIME_OK;
}

bool Permission::declared(const std::string& alias,
                          const std::string& capability) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = declared_.find(alias);
    if (it == declared_.end()) return false;
    return declared_cap(it->second, capability);
}

bool Permission::allowed_locked(const std::string& alias,
                                const std::string& capability) const {
    auto dit = declared_.find(alias);
    if (dit == declared_.end()) return false;
    if (!declared_cap(dit->second, capability)) return false;
    if (capability == "net.insecure" && !allow_insecure_) return false;
    auto pit = per_plugin_.find(alias);
    if (pit != per_plugin_.end()) {
        auto it = pit->second.find(capability);
        if (it != pit->second.end()) return it->second;
    }
    auto git = global_.find(capability);
    if (git != global_.end()) return git->second;
    return false;
}

bool Permission::allowed(const std::string& alias,
                         const std::string& capability) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allowed_locked(alias, capability);
}

bool Permission::app_allowed(const std::string& alias) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!allowed_locked(alias, "context.read")) return false;
    auto pit = per_plugin_.find(alias);
    if (pit != per_plugin_.end()) {
        auto it = pit->second.find("context.read.app");
        if (it != pit->second.end()) return it->second;
    }
    auto git = global_.find("context.read.app");
    return git != global_.end() && git->second;
}

std::vector<std::string> Permission::granted(const std::string& alias) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    auto dit = declared_.find(alias);
    if (dit == declared_.end()) return out;
    for (const auto& cap : dit->second) {
        if (allowed_locked(alias, cap)) out.push_back(cap);
    }
    return out;
}

void Permission::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    declared_.clear();
    global_.clear();
    per_plugin_.clear();
}

}  // namespace pime::cap
