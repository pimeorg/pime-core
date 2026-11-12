#include "shared_buffer.hpp"

#include <cstring>

namespace pime::phl {

SharedBuffer::SharedBuffer(size_t size, bool secure)
    : data_(size), secure_(secure) {}

pime_status SharedBuffer::write(uint32_t offset, const void* data, uint32_t len) {
    if (!data && len != 0) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<uint64_t>(offset) + len > data_.size()) return PIME_INVALID;
    if (len) std::memcpy(data_.data() + offset, data, len);
    return PIME_OK;
}

pime_status SharedBuffer::read(uint32_t offset, void* out, uint32_t cap,
                               uint32_t* out_len) const {
    if (!out_len) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset > data_.size()) return PIME_INVALID;
    uint32_t avail = static_cast<uint32_t>(data_.size() - offset);
    *out_len = avail;
    if (avail > cap) return PIME_BUFFER_TOO_SMALL;
    if (avail && !out) return PIME_INVALID;
    if (avail) std::memcpy(out, data_.data() + offset, avail);
    return PIME_OK;
}

pime_status SharedBuffer::resize(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_count_ > 0) return PIME_BUSY;
    data_.resize(size);
    return PIME_OK;
}

pime_status SharedBuffer::map(void** ptr, uint32_t* size) {
    if (!ptr || !size) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    ++map_count_;
    *ptr = data_.data();
    *size = static_cast<uint32_t>(data_.size());
    return PIME_OK;
}

pime_status SharedBuffer::unmap() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_count_ == 0) return PIME_INVALID;
    --map_count_;
    return PIME_OK;
}

uint32_t SharedBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(data_.size());
}

bool SharedBuffer::mapped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_count_ > 0;
}

void SharedBuffer::wipe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data_.empty()) std::memset(data_.data(), 0, data_.size());
}

}  // namespace pime::phl
