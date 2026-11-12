#include "log.hpp"

#include <cstdio>
#include <cstring>

#include "platform/platform.hpp"
#include "util/string_util.hpp"

namespace pime::cap {

namespace {

void copy_truncated(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, cap, "%s", src);
}

}  // namespace

Log::Log() = default;
Log::~Log() = default;

void Log::set_level(pime_log_level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

pime_log_level Log::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Log::set_plugin_level(const std::string& id, pime_log_level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_levels_[id] = level;
}

bool Log::plugin_allowed(const PluginCapContext& ctx, pime_log_level level,
                         const char* msg, uint32_t* allowed_bytes) {
    pime_log_level max_level = level_;
    auto exact = plugin_levels_.find(ctx.plugin_id + "@" + ctx.version);
    if (exact != plugin_levels_.end()) {
        max_level = exact->second;
    } else {
        auto by_id = plugin_levels_.find(ctx.plugin_id);
        if (by_id != plugin_levels_.end()) max_level = by_id->second;
    }
    if (static_cast<uint32_t>(level) > static_cast<uint32_t>(max_level))
        return false;

    const uint64_t now = platform::monotonic_ms();
    Rate& rate = rates_[ctx.alias];
    if (now - rate.window_start >= 1000) {
        rate.window_start = now;
        rate.bytes = 0;
    }
    uint32_t limit = ctx.limits.log_bytes_per_sec;
    if (limit == 0) limit = 65536;
    uint32_t len = msg ? static_cast<uint32_t>(std::strlen(msg)) : 0;
    if (rate.bytes >= limit) return false;
    if (len > limit - rate.bytes) len = limit - rate.bytes;
    rate.bytes += len;
    *allowed_bytes = len;
    return true;
}

void Log::push_locked(const LogRecord& record) {
    if (count_ == kSlots) {
        ++dropped_;
        ring_[head_] = record;
        head_ = (head_ + 1) % kSlots;
        return;
    }
    ring_[(head_ + count_) % kSlots] = record;
    ++count_;
}

void Log::write(pime_log_level level, const char* source, const char* tag,
                const char* msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<uint32_t>(level) > static_cast<uint32_t>(level_)) return;
    LogRecord record;
    record.time_ms = platform::monotonic_ms();
    record.level = level;
    copy_truncated(record.source, sizeof(record.source), source ? source : "core");
    copy_truncated(record.tag, sizeof(record.tag), tag ? tag : "");
    copy_truncated(record.msg, sizeof(record.msg), msg ? msg : "");
    push_locked(record);
}

void Log::plugin(const PluginCapContext& ctx, pime_log_level level,
                 const char* tag, const char* msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t allowed = 0;
    if (!plugin_allowed(ctx, level, msg, &allowed)) return;
    LogRecord record;
    record.time_ms = platform::monotonic_ms();
    record.level = level;
    copy_truncated(record.source, sizeof(record.source), ctx.source().c_str());
    copy_truncated(record.tag, sizeof(record.tag), tag ? tag : "");
    std::string sanitized = sanitize_log_text(msg ? std::string_view(msg, allowed) : std::string_view(), 1024);
    copy_truncated(record.msg, sizeof(record.msg), sanitized.c_str());
    push_locked(record);
}

void Log::pump(pime_host_sink* sink, void* ud) {
    if (!sink || !sink->on_log) return;
    LogRecord batch[kSlots];
    size_t n = 0;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        n = count_;
        dropped = dropped_;
        for (size_t i = 0; i < n; ++i) batch[i] = ring_[(head_ + i) % kSlots];
        head_ = (head_ + n) % kSlots;
        count_ = 0;
        dropped_ = 0;
    }
    if (dropped > 0 && n > 0) {
        /* Attach the suppressed count to the first record without allocation. */
        char extra[64];
        std::snprintf(extra, sizeof(extra), " [%llu suppressed]",
                      static_cast<unsigned long long>(dropped));
        std::strncat(batch[0].msg, extra,
                     sizeof(batch[0].msg) - std::strlen(batch[0].msg) - 1);
    }
    for (size_t i = 0; i < n; ++i) {
        pime_log_record record{};
        record.size = sizeof(record);
        record.version = PIME_ABI_MAJOR;
        record.time_ms = batch[i].time_ms;
        record.level = batch[i].level;
        record.source = batch[i].source;
        record.source_len = static_cast<uint32_t>(std::strlen(batch[i].source));
        record.tag = batch[i].tag;
        record.tag_len = static_cast<uint32_t>(std::strlen(batch[i].tag));
        record.msg = batch[i].msg;
        record.msg_len = static_cast<uint32_t>(std::strlen(batch[i].msg));
        sink->on_log(ud, &record);
    }
}

uint64_t Log::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

}  // namespace pime::cap
