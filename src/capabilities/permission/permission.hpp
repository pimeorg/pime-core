#ifndef PIME_CAPABILITIES_PERMISSION_HPP
#define PIME_CAPABILITIES_PERMISSION_HPP

#include <pime/abi.h>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "codec/cbor.hpp"

namespace pime::cap {

/* Declared capabilities are denied until the host grants them. */
class Permission {
public:
    explicit Permission(bool allow_insecure = false);

    void declare(const std::string& alias, const std::vector<std::string>& caps);
    void undeclare(const std::string& alias);

    /* spec may be "cap", "cap(detail)", or "alias:cap". */
    pime_status grant(const std::string& spec, bool granted);

    bool declared(const std::string& alias, const std::string& capability) const;
    bool allowed(const std::string& alias, const std::string& capability) const;
    bool app_allowed(const std::string& alias) const;

    /* Granted capability names for caps.query. */
    std::vector<std::string> granted(const std::string& alias) const;

    void clear();

private:
    bool allowed_locked(const std::string& alias, const std::string& capability) const;
    mutable std::mutex mutex_;
    bool allow_insecure_;
    std::map<std::string, std::set<std::string>> declared_;
    std::map<std::string, bool> global_;
    std::map<std::string, std::map<std::string, bool>> per_plugin_;
};

}  // namespace pime::cap

#endif
