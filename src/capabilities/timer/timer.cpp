#include "timer.hpp"

#include <chrono>

#include "platform/platform.hpp"

namespace pime::cap {

TimerService::TimerService() = default;

TimerService::~TimerService() { stop(); }

void TimerService::start(Callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    callback_ = std::move(callback);
    running_ = true;
    stopping_ = false;
    thread_ = std::thread([this] { loop(); });
}

void TimerService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    stopping_ = false;
    queue_ = {};
    timers_.clear();
    by_event_.clear();
}

void TimerService::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = true;
}

void TimerService::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!paused_) return;
    paused_ = false;
    const uint64_t now = platform::monotonic_ms();
    while (!queue_.empty()) queue_.pop();
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.due_ms <= now) {
            if (it->second.repeat_ms > 0) {
                it->second.due_ms = now + it->second.repeat_ms;
            } else {
                by_event_.erase(it->second.alias + "\0" + it->second.event_id);
                it = timers_.erase(it);
                continue;
            }
        }
        queue_.push({it->second.due_ms, it->second.id});
        ++it;
    }
    cv_.notify_all();
}

pime_status TimerService::add(const std::string& alias, uint64_t generation,
                              uint32_t delay_ms, uint32_t repeat_ms,
                              const std::string& event_id, uint32_t* timer_id) {
    if (!timer_id) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = alias + "\0" + event_id;
    auto existing = by_event_.find(key);
    if (existing != by_event_.end()) {
        auto it = timers_.find(existing->second);
        if (it != timers_.end()) it->second.cancelled = true;
        by_event_.erase(existing);
    }
    Timer timer;
    timer.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    timer.alias = alias;
    timer.generation = generation;
    timer.event_id = event_id;
    timer.due_ms = platform::monotonic_ms() + delay_ms;
    timer.repeat_ms = repeat_ms;
    const uint32_t id = timer.id;
    timers_[id] = timer;
    by_event_[key] = id;
    queue_.push({timer.due_ms, id});
    cv_.notify_all();
    *timer_id = id;
    return PIME_OK;
}

pime_status TimerService::cancel(uint32_t timer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = timers_.find(timer_id);
    if (it == timers_.end()) return PIME_NOT_FOUND;
    it->second.cancelled = true;
    by_event_.erase(it->second.alias + "\0" + it->second.event_id);
    timers_.erase(it);
    return PIME_OK;
}

void TimerService::cancel_generation(const std::string& alias, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.alias == alias && it->second.generation == generation) {
            by_event_.erase(it->second.alias + "\0" + it->second.event_id);
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
}

void TimerService::cancel_alias(const std::string& alias) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.alias == alias) {
            by_event_.erase(it->second.alias + "\0" + it->second.event_id);
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
}

void TimerService::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (paused_ || queue_.empty()) {
            cv_.wait(lock, [&] { return stopping_ || (!paused_ && !queue_.empty()); });
            continue;
        }
        const uint64_t now = platform::monotonic_ms();
        const auto top = queue_.top();
        if (top.first > now) {
            cv_.wait_for(lock, std::chrono::milliseconds(top.first - now));
            continue;
        }
        queue_.pop();
        auto it = timers_.find(top.second);
        if (it == timers_.end() || it->second.cancelled || it->second.due_ms != top.first)
            continue;
        Timer timer = it->second;
        if (timer.repeat_ms > 0) {
            timer.due_ms = now + timer.repeat_ms;
            it->second.due_ms = timer.due_ms;
            queue_.push({timer.due_ms, timer.id});
        } else {
            timers_.erase(it);
            by_event_.erase(timer.alias + "\0" + timer.event_id);
        }
        Callback cb = callback_;
        lock.unlock();
        if (cb) cb(TimerFire{timer.id, timer.alias, timer.generation, timer.event_id});
        lock.lock();
    }
}

}  // namespace pime::cap
