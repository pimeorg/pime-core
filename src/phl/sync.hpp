#ifndef PIME_PHL_SYNC_HPP
#define PIME_PHL_SYNC_HPP

#include <pime/abi.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pime::phl {

constexpr uint32_t kInfiniteMs = 0xFFFFFFFFu;

/* Non-recursive by default. A thread that exits while holding the lock
 * poisons it; it is not silently unlocked. */
class Mutex {
public:
    explicit Mutex(bool recursive = false);
    ~Mutex();

    pime_status lock(uint32_t timeout_ms = kInfiniteMs);
    pime_status try_lock();
    pime_status unlock();

    bool recursive() const { return recursive_; }
    bool poisoned() const;

    void on_owner_exit(std::thread::id id);

private:
    friend class SyncRegistry;
    mutable std::mutex internal_;
    std::condition_variable cv_;
    std::thread::id owner_{};
    int depth_ = 0;
    bool recursive_ = false;
    bool poisoned_ = false;
};

class Condvar {
public:
    Condvar() = default;
    pime_status wait(Mutex* mutex, uint32_t timeout_ms = kInfiniteMs);
    void signal();
    void broadcast();
    void close();
    bool closed() const;

private:
    mutable std::mutex internal_;
    std::condition_variable cv_;
    uint64_t sequence_ = 0;
    bool closed_ = false;
};

class SyncRegistry {
public:
    static void register_mutex(const std::weak_ptr<Mutex>& mutex);
    /* Called when a plugin thread ends, to poison mutexes it left held. */
    static void on_thread_exit(std::thread::id id);
    static void clear();
};

}  // namespace pime::phl

#endif
