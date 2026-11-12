#ifndef PIME_PHL_CHANNEL_HPP
#define PIME_PHL_CHANNEL_HPP

#include <pime/abi.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "sync.hpp"

namespace pime::phl {

enum class MessageMode : uint32_t { Inline = 0, Buffer = 1, Shm = 2 };

struct Message {
    MessageMode mode = MessageMode::Inline;
    std::vector<uint8_t> data; /* inline payload */
    pime_buffer handle = 0;    /* BUFFER or SHM backing */
    uint32_t offset = 0;
    uint32_t length = 0;
};

/* Multi-producer / multi-consumer FIFO. capacity 0 is unbounded. */
class Channel {
public:
    explicit Channel(uint32_t capacity);

    pime_status send(const Message& message, uint32_t timeout_ms = 0);
    pime_status recv(Message* message, uint32_t timeout_ms = kInfiniteMs);
    pime_status close(bool abort = false);

    bool closed() const;
    size_t depth() const;
    uint32_t capacity() const { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<Message> queue_;
    uint32_t capacity_;
    bool closed_ = false;
    bool aborted_ = false;
};

}  // namespace pime::phl

#endif
