#ifndef PIME_PHL_THREAD_HPP
#define PIME_PHL_THREAD_HPP

#include <pime/abi.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pime::phl {

enum class ThreadPriority : uint32_t {
    Background = 0,
    Normal = 1,
    Interactive = 2
};

struct ThreadDesc {
    std::string alias;
    std::string entry;
    std::string name;
    uint32_t stack_kb = 0;
    uint32_t priority = static_cast<uint32_t>(ThreadPriority::Normal);
    uint32_t max_frame_bytes = 65536;
    uint64_t generation = 0;
};

/* A plugin-owned thread. The host runtime supplies the entry function.
 * Threads are retained until close/join, never detached by the plugin API. */
class PluginThread : public std::enable_shared_from_this<PluginThread> {
public:
    using Entry = std::function<void(PluginThread&)>;

    PluginThread(ThreadDesc desc, Entry entry);
    ~PluginThread();

    pime_status start();
    pime_status post(const void* data, uint32_t len, uint32_t timeout_ms = 0);
    pime_status recv(void* out, uint32_t cap, uint32_t* out_len,
                     uint32_t timeout_ms);
    pime_status park(uint32_t timeout_ms);
    void unpark();
    pime_status close();
    pime_status join(uint32_t timeout_ms = 0xFFFFFFFFu);

    bool running() const;
    bool closing() const;
    size_t depth() const;
    const ThreadDesc& desc() const { return desc_; }
    uint64_t id() const { return id_; }
    void set_id(uint64_t id) { id_ = id; }
    std::thread::id os_id() const;

private:
    void run();

    ThreadDesc desc_;
    Entry entry_;
    mutable std::mutex mutex_;
    std::condition_variable cv_messages_;
    std::condition_variable cv_park_;
    std::condition_variable cv_done_;
    std::deque<std::vector<uint8_t>> messages_;
    uint32_t queued_bytes_ = 0;
    bool closing_ = false;
    bool parked_ = false;
    bool started_ = false;
    bool finished_ = false;
    uint64_t id_ = 0;
    std::thread thread_;
    std::thread::id os_id_{};
};

}  // namespace pime::phl

#endif
