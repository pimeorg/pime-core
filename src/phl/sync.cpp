#include "sync.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace pime::phl {

namespace {

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::weak_ptr<Mutex>>& registry() {
    static std::vector<std::weak_ptr<Mutex>> v;
    return v;
}

}  // namespace

Mutex::Mutex(bool recursive) : recursive_(recursive) {}

Mutex::~Mutex() = default;

pime_status Mutex::lock(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(internal_);
    const std::thread::id self = std::this_thread::get_id();
    auto ready = [&] {
        return depth_ == 0 || (recursive_ && owner_ == self) ||
               (poisoned_ && owner_ != self);
    };
    if (poisoned_ && owner_ != self) return PIME_POISONED;
    if (ready()) {
        if (depth_ == 0) owner_ = self;
        ++depth_;
        return PIME_OK;
    }
    if (timeout_ms == 0) return PIME_TIMEOUT;
    if (timeout_ms == kInfiniteMs) {
        cv_.wait(lock, ready);
    } else {
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready))
            return PIME_TIMEOUT;
    }
    if (poisoned_ && owner_ != self) return PIME_POISONED;
    if (depth_ == 0) owner_ = self;
    ++depth_;
    return PIME_OK;
}

pime_status Mutex::try_lock() { return lock(0); }

pime_status Mutex::unlock() {
    std::lock_guard<std::mutex> lock(internal_);
    const std::thread::id self = std::this_thread::get_id();
    if (depth_ == 0 || owner_ != self) return PIME_INVALID;
    --depth_;
    if (depth_ == 0) {
        owner_ = std::thread::id{};
        cv_.notify_one();
    }
    return PIME_OK;
}

bool Mutex::poisoned() const {
    std::lock_guard<std::mutex> lock(internal_);
    return poisoned_;
}

void Mutex::on_owner_exit(std::thread::id id) {
    std::lock_guard<std::mutex> lock(internal_);
    if (depth_ > 0 && owner_ == id) {
        poisoned_ = true;
        cv_.notify_all();
    }
}

pime_status Condvar::wait(Mutex* mutex, uint32_t timeout_ms) {
    if (!mutex) return PIME_INVALID;
    uint64_t observed;
    {
        std::lock_guard<std::mutex> lock(internal_);
        if (closed_) return PIME_CLOSED;
        observed = sequence_;
    }
    pime_status st = mutex->unlock();
    if (st != PIME_OK) return st;

    pime_status result = PIME_OK;
    {
        std::unique_lock<std::mutex> lock(internal_);
        auto ready = [&] { return closed_ || sequence_ != observed; };
        if (timeout_ms == kInfiniteMs) {
            cv_.wait(lock, ready);
        } else if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            result = PIME_TIMEOUT;
        }
        if (closed_) result = PIME_CLOSED;
    }
    pime_status relock = mutex->lock();
    if (relock != PIME_OK) return relock;
    return result;
}

void Condvar::signal() {
    {
        std::lock_guard<std::mutex> lock(internal_);
        ++sequence_;
    }
    cv_.notify_one();
}

void Condvar::broadcast() {
    {
        std::lock_guard<std::mutex> lock(internal_);
        ++sequence_;
    }
    cv_.notify_all();
}

void Condvar::close() {
    {
        std::lock_guard<std::mutex> lock(internal_);
        closed_ = true;
        ++sequence_;
    }
    cv_.notify_all();
}

bool Condvar::closed() const {
    std::lock_guard<std::mutex> lock(internal_);
    return closed_;
}

void SyncRegistry::register_mutex(const std::weak_ptr<Mutex>& mutex) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& v = registry();
    if (v.size() > 1024) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const auto& w) { return w.expired(); }),
                v.end());
    }
    v.push_back(mutex);
}

void SyncRegistry::on_thread_exit(std::thread::id id) {
    std::vector<std::shared_ptr<Mutex>> alive;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto& v = registry();
        for (auto it = v.begin(); it != v.end();) {
            auto ptr = it->lock();
            if (!ptr) {
                it = v.erase(it);
            } else {
                alive.push_back(std::move(ptr));
                ++it;
            }
        }
    }
    for (auto& m : alive) m->on_owner_exit(id);
}

void SyncRegistry::clear() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().clear();
}

}  // namespace pime::phl
