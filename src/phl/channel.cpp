#include "channel.hpp"

#include <chrono>

namespace pime::phl {

Channel::Channel(uint32_t capacity) : capacity_(capacity) {}

pime_status Channel::send(const Message& message, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto has_room = [&] {
        return closed_ || capacity_ == 0 || queue_.size() < capacity_;
    };
    if (closed_) return PIME_CLOSED;
    if (!has_room()) {
        if (timeout_ms == 0) return PIME_FULL;
        if (timeout_ms == kInfiniteMs) {
            cv_not_full_.wait(lock, has_room);
        } else if (!cv_not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                          has_room)) {
            return PIME_FULL;
        }
        if (closed_) return PIME_CLOSED;
    }
    queue_.push_back(message);
    cv_not_empty_.notify_one();
    return PIME_OK;
}

pime_status Channel::recv(Message* message, uint32_t timeout_ms) {
    if (!message) return PIME_INVALID;
    std::unique_lock<std::mutex> lock(mutex_);
    auto ready = [&] { return closed_ || !queue_.empty(); };
    if (!ready()) {
        if (timeout_ms == 0) return PIME_EMPTY;
        if (timeout_ms == kInfiniteMs) {
            cv_not_empty_.wait(lock, ready);
        } else if (!cv_not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                           ready)) {
            return PIME_TIMEOUT;
        }
    }
    if (queue_.empty()) return PIME_CLOSED;
    *message = std::move(queue_.front());
    queue_.pop_front();
    cv_not_full_.notify_one();
    return PIME_OK;
}

pime_status Channel::close(bool abort) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return PIME_INVALID;
        closed_ = true;
        aborted_ = abort;
        if (abort) queue_.clear();
    }
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
    return PIME_OK;
}

bool Channel::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

size_t Channel::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace pime::phl
