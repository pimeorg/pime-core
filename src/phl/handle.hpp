#ifndef PIME_PHL_HANDLE_HPP
#define PIME_PHL_HANDLE_HPP

#include <pime/abi.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pime::phl {

inline uint64_t make_handle(uint32_t index, uint32_t generation) {
    return (static_cast<uint64_t>(generation) << 32) | index;
}

inline uint32_t handle_index(uint64_t handle) {
    return static_cast<uint32_t>(handle & 0xFFFFFFFFull);
}

inline uint32_t handle_generation(uint64_t handle) {
    return static_cast<uint32_t>(handle >> 32);
}

/* Index + generation handles over shared objects. Thread safe. */
template <typename T>
class HandleRegistry {
public:
    uint64_t insert(std::shared_ptr<T> object) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = next_++;
            if (index == 0) return 0;
        }
        Entry& entry = entries_[index];
        if (entry.generation == 0) entry.generation = 1;
        entry.object = std::move(object);
        return make_handle(index, entry.generation);
    }

    std::shared_ptr<T> get(uint64_t handle, pime_status* status = nullptr) const {
        auto set = [status](pime_status s) {
            if (status) *status = s;
        };
        if (handle == 0) {
            set(PIME_INVALID);
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(handle_index(handle));
        if (it == entries_.end()) {
            set(PIME_NOT_FOUND);
            return nullptr;
        }
        if (it->second.generation != handle_generation(handle)) {
            set(PIME_STALE);
            return nullptr;
        }
        if (!it->second.object) {
            set(PIME_STALE);
            return nullptr;
        }
        set(PIME_OK);
        return it->second.object;
    }

    pime_status erase(uint64_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(handle_index(handle));
        if (it == entries_.end()) return PIME_NOT_FOUND;
        if (it->second.generation != handle_generation(handle)) return PIME_STALE;
        if (!it->second.object) return PIME_STALE;
        it->second.object.reset();
        ++it->second.generation;
        if (it->second.generation == 0) it->second.generation = 1;
        free_.push_back(handle_index(handle));
        return PIME_OK;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        free_.clear();
        next_ = 1;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size() - free_.size();
    }

private:
    struct Entry {
        uint32_t generation = 1;
        std::shared_ptr<T> object;
    };
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, Entry> entries_;
    std::vector<uint32_t> free_;
    uint32_t next_ = 1;
};

}  // namespace pime::phl

#endif
