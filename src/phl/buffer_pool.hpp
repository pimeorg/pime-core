#ifndef PIME_PHL_BUFFER_POOL_HPP
#define PIME_PHL_BUFFER_POOL_HPP

#include <pime/abi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace pime::phl {

enum class BufferKind : uint32_t {
    Pcm = 0,
    Opus,
    Net,
    NetLarge,
    Candidate,
    Text,
    Log,
    Count
};

struct BufferStats {
    std::atomic<uint64_t> acquires{0};
    std::atomic<uint64_t> reuses{0};
    std::atomic<uint64_t> drops{0};
    std::atomic<uint64_t> backpressure{0};
};

struct BufferView {
    uint64_t handle = 0;
    void* data = nullptr;
    uint32_t capacity = 0;
    uint32_t length = 0;
};

class BufferPool {
public:
    explicit BufferPool(uint64_t memory_budget_bytes = 0);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    /* Capacity is fixed per kind; wanted larger than the class fails. */
    pime_status acquire(BufferKind kind, uint64_t session_id, uint32_t wanted,
                        bool secure, BufferView* out);
    pime_status retain(uint64_t handle, BufferView* out);
    pime_status release(uint64_t handle);
    pime_status set_length(uint64_t handle, uint32_t length);
    uint32_t length(uint64_t handle) const;
    uint32_t capacity(uint64_t handle) const;
    pime_status write(uint64_t handle, uint32_t offset, const void* data,
                      uint32_t len);
    pime_status read(uint64_t handle, uint32_t offset, void* out,
                     uint32_t cap, uint32_t* out_len) const;

    uint64_t reserved_bytes() const { return reserved_bytes_; }
    uint64_t in_use_bytes() const;
    const BufferStats& stats() const { return stats_; }

    /* Invalidates every handle (generation switch / secure wipe). */
    void invalidate_all();

    /* Frees every slot owned by the session. */
    void release_session(uint64_t session_id);

    static uint32_t class_capacity(BufferKind kind);
    static uint32_t class_slots(BufferKind kind);

private:
    struct Slot {
        BufferKind kind = BufferKind::Pcm;
        std::vector<uint8_t> storage;
        std::atomic<uint32_t> refs{0};
        uint32_t length = 0;
        uint32_t generation = 1;
        uint64_t session_id = 0;
        bool in_use = false;
        bool ever_used = false;
        bool secure = false;
    };

    Slot* slot_for(uint64_t handle, pime_status* status) const;
    void put_free(uint32_t index);

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_[static_cast<size_t>(BufferKind::Count)];
    uint64_t reserved_bytes_ = 0;
    BufferStats stats_;
};

}  // namespace pime::phl

#endif
