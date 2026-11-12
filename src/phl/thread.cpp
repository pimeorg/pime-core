#include "thread.hpp"

#include <chrono>
#include <cstring>

#include "sync.hpp"

namespace pime::phl {

PluginThread::PluginThread(ThreadDesc desc, Entry entry)
    : desc_(std::move(desc)), entry_(std::move(entry)) {}

PluginThread::~PluginThread() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        if (thread_.joinable() && finished_) thread_.detach();
    }
    cv_messages_.notify_all();
    cv_park_.notify_all();
}

pime_status PluginThread::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return PIME_BUSY;
    started_ = true;
    auto self = shared_from_this();
    thread_ = std::thread([self] { self->run(); });
    os_id_ = thread_.get_id();
    return PIME_OK;
}

void PluginThread::run() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        os_id_ = std::this_thread::get_id();
    }
    if (entry_) entry_(*this);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        if (thread_.joinable()) thread_.detach();
    }
    cv_done_.notify_all();
    SyncRegistry::on_thread_exit(std::this_thread::get_id());
}

pime_status PluginThread::post(const void* data, uint32_t len,
                               uint32_t timeout_ms) {
    if (!data && len != 0) return PIME_INVALID;
    if (len > desc_.max_frame_bytes) return PIME_BUFFER_TOO_SMALL;
    std::unique_lock<std::mutex> lock(mutex_);
    auto has_room = [&] {
        return closing_ || queued_bytes_ + len <= desc_.max_frame_bytes * 4;
    };
    if (closing_) return PIME_CLOSED;
    if (!has_room()) {
        if (timeout_ms == 0) return PIME_FULL;
        if (!cv_messages_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   has_room))
            return PIME_FULL;
        if (closing_) return PIME_CLOSED;
    }
    messages_.emplace_back(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + len);
    queued_bytes_ += len;
    cv_messages_.notify_one();
    return PIME_OK;
}

pime_status PluginThread::recv(void* out, uint32_t cap, uint32_t* out_len,
                               uint32_t timeout_ms) {
    if (!out_len) return PIME_INVALID;
    std::unique_lock<std::mutex> lock(mutex_);
    auto ready = [&] { return closing_ || !messages_.empty(); };
    if (!ready()) {
        if (timeout_ms == 0) return PIME_EMPTY;
        if (timeout_ms == kInfiniteMs) {
            cv_messages_.wait(lock, ready);
        } else if (!cv_messages_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                          ready)) {
            return PIME_TIMEOUT;
        }
    }
    if (messages_.empty()) return PIME_CLOSED;
    std::vector<uint8_t>& msg = messages_.front();
    *out_len = static_cast<uint32_t>(msg.size());
    if (msg.size() > cap) return PIME_BUFFER_TOO_SMALL;
    if (!msg.empty()) std::memcpy(out, msg.data(), msg.size());
    queued_bytes_ -= static_cast<uint32_t>(msg.size());
    messages_.pop_front();
    cv_messages_.notify_all();
    return PIME_OK;
}

pime_status PluginThread::park(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closing_) return PIME_CLOSED;
    parked_ = true;
    auto ready = [&] { return !parked_ || closing_; };
    if (timeout_ms == kInfiniteMs) {
        cv_park_.wait(lock, ready);
    } else if (!cv_park_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
        parked_ = false;
        return PIME_TIMEOUT;
    }
    parked_ = false;
    return closing_ ? PIME_CLOSED : PIME_OK;
}

void PluginThread::unpark() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        parked_ = false;
    }
    cv_park_.notify_all();
}

pime_status PluginThread::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    cv_messages_.notify_all();
    cv_park_.notify_all();
    return PIME_OK;
}

pime_status PluginThread::join(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return PIME_INVALID;
    auto done = [&] { return finished_; };
    if (finished_) return PIME_OK;
    if (timeout_ms == kInfiniteMs) {
        cv_done_.wait(lock, done);
    } else if (!cv_done_.wait_for(lock, std::chrono::milliseconds(timeout_ms), done)) {
        return PIME_TIMEOUT;
    }
    return PIME_OK;
}

bool PluginThread::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && !finished_;
}

bool PluginThread::closing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_;
}

size_t PluginThread::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.size();
}

std::thread::id PluginThread::os_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return os_id_;
}

}  // namespace pime::phl
