#ifndef PIME_CAPABILITIES_LOG_HPP
#define PIME_CAPABILITIES_LOG_HPP

#include <pime/hal.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cap_context.hpp"

namespace pime::cap {

struct LogRecord {
    uint64_t time_ms = 0;
    uint32_t level = PIME_LOG_INFO;
    char source[128] = {};
    char tag[64] = {};
    char msg[1024] = {};
};

/* Fixed 64 x 1KB MPSC ring; producers never touch I/O. */
class Log {
public:
    static constexpr size_t kSlots = 64;

    Log();
    ~Log();

    void set_level(pime_log_level level);
    pime_log_level level() const;
    void set_plugin_level(const std::string& id, pime_log_level level);

    void write(pime_log_level level, const char* source, const char* tag,
               const char* msg);
    void plugin(const PluginCapContext& ctx, pime_log_level level,
                const char* tag, const char* msg);

    /* Owner thread: delivers queued records through the sink. */
    void pump(pime_host_sink* sink, void* ud);

    uint64_t dropped() const;

private:
    struct Rate {
        uint64_t window_start = 0;
        uint32_t bytes = 0;
    };

    bool plugin_allowed(const PluginCapContext& ctx, pime_log_level level,
                        const char* msg, uint32_t* allowed_bytes);
    void push_locked(const LogRecord& record);

    mutable std::mutex mutex_;
    std::array<LogRecord, kSlots> ring_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t dropped_ = 0;
    pime_log_level level_ = PIME_LOG_INFO;
    std::unordered_map<std::string, pime_log_level> plugin_levels_;
    std::unordered_map<std::string, Rate> rates_;
};

}  // namespace pime::cap

#endif
