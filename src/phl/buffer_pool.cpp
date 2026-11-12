#include "buffer_pool.hpp"

#include <cstring>
#include <memory>

#include "phl/handle.hpp"

namespace pime::phl {

namespace {

struct ClassSpec {
    uint32_t capacity;
    uint32_t slots;
};

constexpr ClassSpec kClasses[] = {
    {16u * 1024u, 8},          /* PCM 40ms @ 48k/2ch/32bit */
    {2u * 1024u, 32},          /* Opus */
    {64u * 1024u, 4},          /* network */
    {1024u * 1024u, 2},        /* large network */
    {64u * 1024u, 64},         /* candidate snapshot (2 per session enforced) */
    {4u * 1024u, 8},           /* text */
    {1024u, 64},               /* log slot */
};

}  // namespace

uint32_t BufferPool::class_capacity(BufferKind kind) {
    uint32_t i = static_cast<uint32_t>(kind);
    return i < static_cast<uint32_t>(BufferKind::Count) ? kClasses[i].capacity : 0;
}

uint32_t BufferPool::class_slots(BufferKind kind) {
    uint32_t i = static_cast<uint32_t>(kind);
    return i < static_cast<uint32_t>(BufferKind::Count) ? kClasses[i].slots : 0;
}

BufferPool::BufferPool(uint64_t memory_budget_bytes) {
    (void)memory_budget_bytes;
    size_t total_slots = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(BufferKind::Count); ++i) {
        total_slots += kClasses[i].slots;
        reserved_bytes_ += static_cast<uint64_t>(kClasses[i].capacity) * kClasses[i].slots;
    }
    slots_ = std::vector<Slot>(total_slots);
    size_t index = 0;
    for (uint32_t kind = 0; kind < static_cast<uint32_t>(BufferKind::Count); ++kind) {
        for (uint32_t n = 0; n < kClasses[kind].slots; ++n) {
            Slot& slot = slots_[index];
            slot.kind = static_cast<BufferKind>(kind);
            slot.storage.resize(kClasses[kind].capacity);
            free_[kind].push_back(static_cast<uint32_t>(index));
            ++index;
        }
    }
}

BufferPool::~BufferPool() = default;

BufferPool::Slot* BufferPool::slot_for(uint64_t handle, pime_status* status) const {
    auto set = [status](pime_status s) {
        if (status) *status = s;
    };
    if (handle == 0) {
        set(PIME_INVALID);
        return nullptr;
    }
    const uint32_t index = handle_index(handle);
    if (index >= slots_.size()) {
        set(PIME_NOT_FOUND);
        return nullptr;
    }
    Slot& slot = const_cast<Slot&>(slots_[index]);
    if (slot.generation != handle_generation(handle)) {
        set(PIME_STALE);
        return nullptr;
    }
    if (!slot.in_use) {
        set(PIME_STALE);
        return nullptr;
    }
    set(PIME_OK);
    return &slot;
}

void BufferPool::put_free(uint32_t index) {
    Slot& slot = slots_[index];
    slot.in_use = false;
    slot.refs.store(0, std::memory_order_release);
    slot.length = 0;
    slot.session_id = 0;
    slot.secure = false;
    ++slot.generation;
    if (slot.generation == 0) slot.generation = 1;
    free_[static_cast<size_t>(slot.kind)].push_back(index);
}

pime_status BufferPool::acquire(BufferKind kind, uint64_t session_id,
                                uint32_t wanted, bool secure, BufferView* out) {
    if (!out) return PIME_INVALID;
    const uint32_t kind_index = static_cast<uint32_t>(kind);
    if (kind_index >= static_cast<uint32_t>(BufferKind::Count)) return PIME_INVALID;
    if (wanted > kClasses[kind_index].capacity) return PIME_BUFFER_TOO_SMALL;

    std::lock_guard<std::mutex> lock(mutex_);
    if (kind == BufferKind::Candidate && session_id != 0) {
        uint32_t used = 0;
        for (const Slot& slot : slots_) {
            if (slot.kind == BufferKind::Candidate && slot.in_use &&
                slot.session_id == session_id)
                ++used;
        }
        if (used >= 2) {
            ++stats_.backpressure;
            return PIME_FULL;
        }
    }
    if (free_[kind_index].empty()) {
        ++stats_.backpressure;
        return PIME_NO_MEMORY;
    }
    const uint32_t index = free_[kind_index].back();
    free_[kind_index].pop_back();
    Slot& slot = slots_[index];
    slot.in_use = true;
    slot.refs.store(1, std::memory_order_release);
    slot.length = 0;
    slot.session_id = session_id;
    slot.secure = secure;
    if (secure) std::memset(slot.storage.data(), 0, slot.storage.size());
    if (slot.ever_used) ++stats_.reuses;
    slot.ever_used = true;
    ++stats_.acquires;

    out->handle = make_handle(index, slot.generation);
    out->data = slot.storage.data();
    out->capacity = static_cast<uint32_t>(slot.storage.size());
    out->length = 0;
    return PIME_OK;
}

pime_status BufferPool::retain(uint64_t handle, BufferView* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    if (!slot) return PIME_STALE;
    slot->refs.fetch_add(1, std::memory_order_acq_rel);
    if (out) {
        out->handle = handle;
        out->data = slot->storage.data();
        out->capacity = static_cast<uint32_t>(slot->storage.size());
        out->length = slot->length;
    }
    return PIME_OK;
}

pime_status BufferPool::release(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    if (!slot) return PIME_STALE;
    uint32_t prev = slot->refs.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 0) {
        slot->refs.store(0);
        return PIME_STALE;
    }
    if (prev == 1) {
        if (slot->secure || slot->kind == BufferKind::Pcm) {
            std::memset(slot->storage.data(), 0, slot->storage.size());
        }
        put_free(static_cast<uint32_t>(slot - slots_.data()));
    }
    return PIME_OK;
}

pime_status BufferPool::set_length(uint64_t handle, uint32_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    if (!slot) return PIME_STALE;
    if (length > slot->storage.size()) return PIME_BUFFER_TOO_SMALL;
    slot->length = length;
    return PIME_OK;
}

uint32_t BufferPool::length(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    return slot ? slot->length : 0;
}

uint32_t BufferPool::capacity(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    return slot ? static_cast<uint32_t>(slot->storage.size()) : 0;
}

pime_status BufferPool::write(uint64_t handle, uint32_t offset, const void* data,
                              uint32_t len) {
    if (!data && len != 0) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    if (!slot) return PIME_STALE;
    if (static_cast<uint64_t>(offset) + len > slot->storage.size())
        return PIME_INVALID;
    if (len) std::memcpy(slot->storage.data() + offset, data, len);
    if (offset + len > slot->length) slot->length = offset + len;
    return PIME_OK;
}

pime_status BufferPool::read(uint64_t handle, uint32_t offset, void* out,
                             uint32_t cap, uint32_t* out_len) const {
    if (!out_len) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = slot_for(handle, nullptr);
    if (!slot) return PIME_STALE;
    if (offset > slot->length) return PIME_INVALID;
    uint32_t avail = slot->length - offset;
    *out_len = avail;
    if (avail > cap) return PIME_BUFFER_TOO_SMALL;
    if (avail && !out) return PIME_INVALID;
    if (avail) std::memcpy(out, slot->storage.data() + offset, avail);
    return PIME_OK;
}

uint64_t BufferPool::in_use_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const Slot& slot : slots_) {
        if (slot.in_use) total += slot.storage.size();
    }
    return total;
}

void BufferPool::invalidate_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].in_use) {
            if (slots_[i].secure || slots_[i].kind == BufferKind::Pcm)
                std::memset(slots_[i].storage.data(), 0, slots_[i].storage.size());
            put_free(static_cast<uint32_t>(i));
        }
    }
}

void BufferPool::release_session(uint64_t session_id) {
    if (session_id == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].in_use && slots_[i].session_id == session_id) {
            if (slots_[i].secure || slots_[i].kind == BufferKind::Pcm)
                std::memset(slots_[i].storage.data(), 0, slots_[i].storage.size());
            put_free(static_cast<uint32_t>(i));
        }
    }
}

}  // namespace pime::phl
