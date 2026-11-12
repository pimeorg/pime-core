#include <algorithm>
#include <set>

#include <pime/version.h>

#include "codec/manifest.hpp"
#include "core.hpp"
#include "platform/platform.hpp"
#include "util/string_util.hpp"

namespace pime::core {

namespace {

using codec::Value;

Value make_map() { return Value::map_v(); }
Value make_array() { return Value::array_v(); }

bool valid_alias(const std::string& alias) {
    if (alias.empty() || alias.size() > 128) return false;
    for (char c : alias) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.';
        if (!ok) return false;
    }
    return true;
}

void split_requirement(const std::string& requirement, std::string* name,
                       std::string* constraint) {
    const size_t at = requirement.find('@');
    if (at == std::string::npos) {
        *name = requirement;
        constraint->clear();
        return;
    }
    *name = requirement.substr(0, at);
    *constraint = requirement.substr(at + 1);
}

bool chain_has_version(const Generation& generation, const std::string& id,
                       const std::string& constraint) {
    for (const auto& item : generation.chain) {
        if (item.plugin_id != id) continue;
        if (constraint.empty()) return true;
        if (version_satisfies(parse_semver(item.version), constraint)) return true;
    }
    return false;
}

}  // namespace

pime_status Core::plugin_load(const std::string& package_dir) {
    if (package_dir.empty()) return PIME_INVALID;

    codec::Package scanned;
    pime_status status = packages_->scan(package_dir, &scanned);
    if (status != PIME_OK) return status;
    if (!scanned.valid || !scanned.manifest.valid) return PIME_CORRUPT;
    if (!platform_matches(scanned.manifest)) return PIME_NOT_FOUND;

    InstalledPlugin* existing =
        find_installed(scanned.manifest.id, scanned.manifest.version);
    if (existing) {
        return existing->code_hash == scanned.code_hash ? PIME_OK : PIME_CORRUPT;
    }

    codec::Package package;
    status = packages_->install(package_dir, &package);
    if (status != PIME_OK) return status;
    return add_installed(package);
}

pime_status Core::add_installed(const codec::Package& package) {
    auto plugin = std::make_unique<InstalledPlugin>();
    plugin->id = package.manifest.id;
    plugin->version = package.manifest.version;
    plugin->dir = package.dir.empty()
                      ? packages_->install_path(plugin->id, plugin->version)
                      : package.dir;
    plugin->code_hash = package.code_hash;
    plugin->manifest = package.manifest;
    plugin->enabled = true;

    if (find_installed(plugin->id, plugin->version)) {
        InstalledPlugin* other = find_installed(plugin->id, plugin->version);
        return other->code_hash == plugin->code_hash ? PIME_OK : PIME_CORRUPT;
    }
    installed_.push_back(std::move(plugin));
    return PIME_OK;
}

InstalledPlugin* Core::find_installed(const std::string& id,
                                      const std::string& version) {
    for (auto& plugin : installed_) {
        if (plugin->id == id &&
            (version.empty() || plugin->version == version))
            return plugin.get();
    }
    return nullptr;
}

InstalledPlugin* Core::resolve_installed(const std::string& id,
                                         const std::string& constraint) {
    InstalledPlugin* best = nullptr;
    SemVer best_version;
    for (auto& plugin : installed_) {
        if (plugin->id != id || !plugin->enabled) continue;
        if (!platform_matches(plugin->manifest)) continue;
        const SemVer version = parse_semver(plugin->version);
        if (!version.valid) continue;
        if (!constraint.empty() && !version_satisfies(version, constraint))
            continue;
        if (!best || compare_semver(version, best_version) > 0) {
            best = plugin.get();
            best_version = version;
        }
    }
    return best;
}

bool Core::platform_matches(const codec::Manifest& manifest) const {
    const std::string current = platform::current_platform();
    for (const auto& platform : manifest.platforms) {
        if (platform == current || platform == "any" || platform == "*")
            return true;
    }
    return false;
}

bool Core::parse_setting(const codec::SettingSpec& spec,
                         const codec::Value& value, codec::Value* normalized) {
    if (!normalized) return false;
    if (spec.type == "bool") {
        if (value.type != codec::Type::Bool) return false;
        *normalized = value;
        return true;
    }
    if (spec.type == "int") {
        if (value.type != codec::Type::UInt && value.type != codec::Type::Int)
            return false;
        const int64_t number = value.as_int();
        if (spec.min_value != 0 || spec.max_value != 0) {
            if (number < spec.min_value || number > spec.max_value) return false;
        }
        *normalized = value;
        return true;
    }
    if (spec.type == "string" || spec.type == "enum") {
        if (value.type != codec::Type::Text) return false;
        if (!spec.values.empty()) {
            const std::string text(value.as_text());
            if (std::find(spec.values.begin(), spec.values.end(), text) ==
                spec.values.end())
                return false;
        }
        *normalized = value;
        return true;
    }
    *normalized = value;
    return true;
}

pime_status Core::build_plugin_config(const InstalledPlugin& plugin,
                                      const BuildRequest& request,
                                      codec::Value* out) const {
    if (!out) return PIME_INVALID;
    const Value& provided = request.settings;
    if (!provided.is_map()) return PIME_INVALID;

    Value settings = make_map();
    for (const auto& spec : plugin.manifest.settings) {
        const Value* value = provided.find(spec.key);
        if (value) {
            Value normalized;
            if (!parse_setting(spec, *value, &normalized)) return PIME_INVALID;
            settings.set(spec.key, std::move(normalized));
        } else if (spec.has_default) {
            settings.set(spec.key, spec.default_value);
        }
    }
    for (const auto& entry : provided.map) {
        if (!entry.first.is_text()) return PIME_INVALID;
        const std::string key(entry.first.as_text());
        bool declared = false;
        for (const auto& spec : plugin.manifest.settings) {
            if (spec.key == key) {
                declared = true;
                break;
            }
        }
        if (!declared) return PIME_INVALID;
    }

    const std::string asset_dir = platform::path_join(plugin.dir, "assets");
    Value assets = make_map();
    for (const auto& name : plugin.manifest.assets)
        assets.set(name, Value::text_v(platform::path_join(asset_dir, name)));

    Value abi = make_map();
    abi.set("major", Value::uint_v(plugin.manifest.abi_major));
    abi.set("minor", Value::uint_v(plugin.manifest.abi_minor));

    Value config = make_map();
    config.set("settings", std::move(settings));
    config.set("assets", std::move(assets));
    config.set("abi", std::move(abi));
    *out = std::move(config);
    return PIME_OK;
}

pime_status Core::instantiate(const BuildRequest& request,
                              const InstalledPlugin& plugin,
                              Generation* generation) {
    if (!generation) return PIME_INVALID;
    if (!request.role.empty() && request.role != plugin.manifest.role)
        return PIME_INVALID;
    const std::string alias =
        request.alias.empty() ? plugin.manifest.id : request.alias;
    if (!valid_alias(alias)) return PIME_INVALID;
    for (const auto& item : generation->chain) {
        if (item.alias == alias) return PIME_INVALID;
    }

    Value config;
    pime_status status = build_plugin_config(plugin, request, &config);
    if (status != PIME_OK) return status;

    ChainItem item;
    item.alias = alias;
    item.plugin_id = plugin.manifest.id;
    item.version = plugin.manifest.version;
    item.code_hash = plugin.code_hash;
    item.package_dir = plugin.dir;
    item.manifest = plugin.manifest;
    item.caps.alias = alias;
    item.caps.plugin_id = plugin.manifest.id;
    item.caps.version = plugin.manifest.version;
    item.caps.code_hash = plugin.code_hash;
    item.caps.package_dir = plugin.dir;
    item.caps.asset_dir = platform::path_join(plugin.dir, "assets");
    item.caps.data_dir = options_.data_dir;
    item.caps.generation = generation->id;
    item.caps.state_schema = plugin.manifest.state_schema;
    item.caps.store_schema = plugin.manifest.store_schema;
    item.caps.limits = plugin.manifest.limits;

    std::unique_ptr<host::PluginInstance> instance;
    status = host::create_instance(plugin.manifest, plugin.dir, &item.caps,
                                   this, alias, generation->id, &instance);
    if (status != PIME_OK) return status;
    item.instance = std::move(instance);

    std::vector<uint8_t> encoded;
    codec::encode(config, &encoded);
    status = item.instance->init(encoded);
    if (status != PIME_OK) {
        item.instance->shutdown();
        item.instance.reset();
        return status;
    }
    item.instance->set_session(0, false, 0);

    generation->chain.push_back(std::move(item));
    ChainItem& stored = generation->chain.back();
    stored.caps.manifest = &stored.manifest;
    permission_.declare(alias, plugin.manifest.capabilities);
    return PIME_OK;
}

pime_status Core::resolve_request(
    const BuildRequest& request, const std::string& alias,
    std::vector<BuildRequest>* explicit_requests,
    std::vector<std::string>* visiting, std::vector<std::string>* added,
    Generation* generation) {
    if (!valid_alias(alias)) return PIME_INVALID;
    if (std::find(added->begin(), added->end(), alias) != added->end())
        return PIME_OK;
    if (std::find(visiting->begin(), visiting->end(), request.id) !=
        visiting->end())
        return PIME_NOT_FOUND;

    if (!request.role.empty() && !codec::is_known_role(request.role))
        return PIME_INVALID;

    InstalledPlugin* plugin = resolve_installed(request.id, request.version);
    if (!plugin) return PIME_NOT_FOUND;
    if (!request.role.empty() && request.role != plugin->manifest.role)
        return PIME_INVALID;

    visiting->push_back(request.id);

    for (const auto& requirement : plugin->manifest.requirements) {
        std::string name;
        std::string constraint;
        split_requirement(requirement, &name, &constraint);

        bool plugin_dependency = false;
        for (const auto& installed : installed_) {
            if (installed->id == name) {
                plugin_dependency = true;
                break;
            }
        }
        if (!plugin_dependency) {
            if (!codec::is_known_capability(name)) {
                visiting->pop_back();
                return PIME_NOT_FOUND;
            }
            continue;
        }

        if (chain_has_version(*generation, name, constraint)) continue;

        BuildRequest* explicit_request = nullptr;
        for (auto& candidate : *explicit_requests) {
            if (candidate.id != name) continue;
            InstalledPlugin* resolved =
                resolve_installed(candidate.id, candidate.version);
            if (!resolved) continue;
            if (!constraint.empty() &&
                !version_satisfies(parse_semver(resolved->version), constraint))
                continue;
            explicit_request = &candidate;
            break;
        }

        BuildRequest dependency;
        if (explicit_request) {
            dependency = *explicit_request;
        } else {
            dependency.id = name;
            dependency.version = constraint;
            dependency.alias = name;
        }
        const std::string dep_alias =
            dependency.alias.empty() ? dependency.id : dependency.alias;
        pime_status status =
            resolve_request(dependency, dep_alias, explicit_requests, visiting,
                            added, generation);
        if (status != PIME_OK) {
            visiting->pop_back();
            return status;
        }
    }

    visiting->pop_back();

    pime_status status = instantiate(request, *plugin, generation);
    if (status != PIME_OK) return status;
    added->push_back(alias);
    return PIME_OK;
}

pime_status Core::resolve_chain(const std::vector<BuildRequest>& requests,
                                Generation* generation) {
    std::vector<std::string> visiting;
    std::vector<std::string> added;
    std::vector<BuildRequest> explicit_requests = requests;

    for (const auto& request : requests) {
        const std::string alias =
            request.alias.empty() ? request.id : request.alias;
        pime_status status = resolve_request(request, alias, &explicit_requests,
                                             &visiting, &added, generation);
        if (status != PIME_OK) return status;
    }
    return PIME_OK;
}

codec::Value Core::global_config() const {
    Value config = make_map();
    config.set("core.version",
               Value::text_v(std::to_string(PIME_VERSION_MAJOR) + "." +
                             std::to_string(PIME_VERSION_MINOR) + "." +
                             std::to_string(PIME_VERSION_PATCH)));
    config.set("core.abi_major", Value::uint_v(PIME_ABI_MAJOR));
    config.set("core.abi_minor", Value::uint_v(PIME_ABI_MINOR));
    config.set("core.platform", Value::text_v(platform::current_platform()));
    config.set("core.log_level", Value::uint_v(options_.log_level));
    config.set("core.memory_budget_mb", Value::uint_v(options_.memory_budget_mb));
    return config;
}

pime_status Core::generation_build(const uint8_t* config, uint32_t len,
                                   uint64_t* generation) {
    if (!generation || (!config && len)) return PIME_INVALID;
    if (len == 0) return PIME_INVALID;

    Value root;
    pime_status status = codec::decode(config, len, &root);
    if (status != PIME_OK || !root.is_map()) return PIME_INVALID;
    const Value* list = root.find("plugins");
    if (!list || !list->is_array()) return PIME_INVALID;

    std::vector<BuildRequest> requests;
    std::set<std::string> aliases;
    for (const auto& entry : list->array) {
        if (!entry.is_map()) return PIME_INVALID;
        const Value* id = entry.find("id");
        if (!id || !id->is_text() || id->as_text().empty()) return PIME_INVALID;
        BuildRequest request;
        request.id = std::string(id->as_text());
        if (const Value* version = entry.find("version")) {
            if (!version->is_text()) return PIME_INVALID;
            request.version = std::string(version->as_text());
        }
        if (const Value* alias = entry.find("alias")) {
            if (!alias->is_text()) return PIME_INVALID;
            request.alias = std::string(alias->as_text());
        }
        if (const Value* role = entry.find("role")) {
            if (!role->is_text()) return PIME_INVALID;
            request.role = std::string(role->as_text());
        }
        if (const Value* settings = entry.find("settings")) {
            if (!settings->is_map()) return PIME_INVALID;
            request.settings = *settings;
        }
        const std::string alias =
            request.alias.empty() ? request.id : request.alias;
        if (!valid_alias(alias) || !aliases.insert(alias).second)
            return PIME_INVALID;
        requests.push_back(std::move(request));
    }

    auto built = std::make_unique<Generation>();
    built->id = next_generation_++;
    status = resolve_chain(requests, built.get());
    if (status != PIME_OK) {
        for (const auto& item : built->chain)
            permission_.undeclare(item.alias);
        return status;
    }

    uint32_t workspace_bytes = 1;
    std::map<std::string, std::map<std::string, Value>> plugin_configs;
    for (const auto& item : built->chain) {
        const uint64_t bytes =
            static_cast<uint64_t>(item.caps.limits.workspace_kb) * 1024ull;
        if (bytes > workspace_bytes)
            workspace_bytes =
                static_cast<uint32_t>(std::min<uint64_t>(bytes, 4u * 1024u * 1024u));

        const BuildRequest* request = nullptr;
        for (const auto& candidate : requests) {
            const std::string alias =
                candidate.alias.empty() ? candidate.id : candidate.alias;
            if (alias == item.alias) {
                request = &candidate;
                break;
            }
        }
        BuildRequest fallback_request;
        const BuildRequest* effective = request;
        if (!effective) {
            fallback_request.id = item.plugin_id;
            fallback_request.alias = item.alias;
            effective = &fallback_request;
        }
        InstalledPlugin* plugin =
            find_installed(item.plugin_id, item.version);
        if (!plugin) return PIME_NOT_FOUND;
        Value config;
        status = build_plugin_config(*plugin, *effective, &config);
        if (status != PIME_OK) return status;
        std::map<std::string, Value> settings;
        if (const Value* value = config.find("settings")) {
            for (const auto& entry : value->map) {
                if (entry.first.is_text())
                    settings[std::string(entry.first.as_text())] = entry.second;
            }
        }
        plugin_configs[item.alias] = std::move(settings);
    }
    built->workspace_bytes = workspace_bytes;
    std::map<std::string, Value> global;
    Value global_value = global_config();
    for (auto& entry : global_value.map) {
        if (entry.first.is_text())
            global[std::string(entry.first.as_text())] =
                std::move(entry.second);
    }
    built->config = cap::ConfigSnapshot(built->id, std::move(global),
                                        std::move(plugin_configs));

    const uint64_t id = built->id;
    generations_[id] = std::move(built);
    *generation = id;
    return PIME_OK;
}

pime_status Core::generation_activate(uint64_t generation) {
    auto it = generations_.find(generation);
    if (it == generations_.end()) return PIME_NOT_FOUND;
    if (it->second->retired) return PIME_STALE;
    if (active_generation_ == generation) return PIME_OK;

    Generation* previous =
        active_generation_ ? generations_[active_generation_].get() : nullptr;
    if (previous) {
        std::vector<std::shared_ptr<phl::PluginThread>> threads;
        {
            std::lock_guard<std::mutex> lock(thread_mutex_);
            for (auto entry = threads_.begin(); entry != threads_.end();) {
                if (entry->second->desc().generation == previous->id) {
                    threads.push_back(entry->second);
                    entry = threads_.erase(entry);
                } else {
                    ++entry;
                }
            }
        }
        for (auto& thread : threads) {
            thread->close();
            thread->join(2000);
        }
        for (const auto& item : previous->chain) {
            timers_.cancel_generation(item.alias, previous->id);
            if (net_) net_->cancel_generation(item.alias, previous->id);
            if (ml_) ml_->cancel_generation(item.alias, previous->id);
            for (auto& cache : contexts_)
                cache.second->on_generation_retire(item.alias, item.code_hash);
        }
        buffers_.invalidate_all();
        channels_.clear();
        mutexes_.clear();
        condvars_.clear();
        shared_buffers_.clear();
        phl::SyncRegistry::clear();
    }

    active_generation_ = generation;
    return PIME_OK;
}

pime_status Core::generation_retire(uint64_t generation) {
    auto it = generations_.find(generation);
    if (it == generations_.end()) return PIME_NOT_FOUND;
    if (generation == active_generation_) return PIME_BUSY;

    Generation* retired = it->second.get();
    for (const auto& item : retired->chain) {
        timers_.cancel_generation(item.alias, retired->id);
        if (net_) net_->cancel_generation(item.alias, retired->id);
        if (ml_) ml_->cancel_generation(item.alias, retired->id);
        for (auto& cache : contexts_)
            cache.second->on_generation_retire(item.alias, item.code_hash);
        permission_.undeclare(item.alias);
    }

    std::vector<std::shared_ptr<phl::PluginThread>> threads;
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        for (auto entry = threads_.begin(); entry != threads_.end();) {
            if (entry->second->desc().generation == retired->id) {
                threads.push_back(entry->second);
                entry = threads_.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    for (auto& thread : threads) {
        thread->close();
        thread->join(2000);
    }
    for (auto& item : retired->chain) {
        if (item.instance) item.instance->shutdown();
    }
    generations_.erase(it);
    return PIME_OK;
}

pime_status Core::plugin_unload(const std::string& id,
                                const std::string& version) {
    std::string target = version;
    if (target.empty()) {
        Generation* generation = active_gen();
        if (generation) {
            for (const auto& item : generation->chain) {
                if (item.plugin_id == id) {
                    target = item.version;
                    break;
                }
            }
        }
    }
    if (target.empty()) return PIME_NOT_FOUND;

    InstalledPlugin* plugin = find_installed(id, target);
    if (!plugin) return PIME_NOT_FOUND;
    for (const auto& entry : generations_) {
        for (const auto& item : entry.second->chain) {
            if (item.plugin_id == id && item.version == target) return PIME_BUSY;
        }
    }

    pime_status status = packages_->uninstall(id, target);
    if (status != PIME_OK) return status;
    for (auto it = installed_.begin(); it != installed_.end(); ++it) {
        if ((*it)->id == id && (*it)->version == target) {
            installed_.erase(it);
            break;
        }
    }
    return PIME_OK;
}

pime_status Core::plugin_enable(const std::string& id,
                                const std::string& version, bool enabled) {
    std::string target = version;
    if (target.empty()) {
        Generation* generation = active_gen();
        if (generation) {
            for (const auto& item : generation->chain) {
                if (item.plugin_id == id) {
                    target = item.version;
                    break;
                }
            }
        }
    }
    if (target.empty()) return PIME_NOT_FOUND;

    InstalledPlugin* plugin = find_installed(id, target);
    if (!plugin) return PIME_NOT_FOUND;

    plugin->enabled = enabled;
    for (auto& entry : generations_) {
        for (auto& item : entry.second->chain) {
            if (item.plugin_id == id && item.version == target)
                item.enabled = enabled;
        }
    }
    return PIME_OK;
}

pime_status Core::plugin_list(std::vector<uint8_t>* out) {
    if (!out) return PIME_INVALID;
    Value list = make_array();
    Generation* active = active_gen();
    for (const auto& plugin : installed_) {
        Value entry = make_map();
        entry.set("id", Value::text_v(plugin->id));
        entry.set("version", Value::text_v(plugin->version));
        entry.set("code_hash", Value::text_v(plugin->code_hash));
        bool is_active = false;
        Value generations = make_array();
        for (const auto& generation : generations_) {
            for (const auto& item : generation.second->chain) {
                if (item.plugin_id != plugin->id ||
                    item.version != plugin->version)
                    continue;
                generations.push(Value::uint_v(generation.first));
                if (active && generation.first == active->id) is_active = true;
            }
        }
        entry.set("active", Value::boolean_v(is_active));
        entry.set("enabled", Value::boolean_v(plugin->enabled));
        entry.set("generations", std::move(generations));
        entry.set("state",
                  Value::text_v(plugin->enabled ? "enabled" : "disabled"));
        list.push(std::move(entry));
    }
    out->clear();
    codec::encode(list, out);
    return PIME_OK;
}

}  // namespace pime::core
