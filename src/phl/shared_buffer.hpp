#ifndef PIME_PHL_SHARED_BUFFER_HPP
#define PIME_PHL_SHARED_BUFFER_HPP

#include <pime/abi.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace pime::phl {

class SharedBuffer {
public:
    SharedBuffer(size_t size, bool secure);

    pime_status write(uint32_t offset, const void* data, uint32_t len);
    pime_status read(uint32_t offset, void* out, uint32_t cap,
                     uint32_t* out_len) const;
    pime_status resize(size_t size);
    pime_status map(void** ptr, uint32_t* size);
    pime_status unmap();

    uint32_t size() const;
    bool mapped() const;
    bool secure() const { return secure_; }

    /* Called when the last reference goes away. */
    void wipe();

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> data_;
    uint32_t map_count_ = 0;
    bool secure_ = false;
};

}  // namespace pime::phl

#endif
