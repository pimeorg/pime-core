#ifndef PIME_HOST_LUA_INSTANCE_HPP
#define PIME_HOST_LUA_INSTANCE_HPP

#include <memory>
#include <string>

#include "host/plugin_instance.hpp"

namespace pime::cap {
struct PluginCapContext;
}

namespace pime::host {

/* Defined when PIME_BUILD_LUA is enabled. */
pime_status create_lua_instance(const codec::Manifest& manifest,
                                const std::string& package_dir,
                                const cap::PluginCapContext* caps,
                                HostBridge* bridge, const std::string& alias,
                                uint64_t generation,
                                std::unique_ptr<PluginInstance>* out);

}  // namespace pime::host

#endif
