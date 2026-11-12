#include "stream.hpp"

#include <chrono>
#include <thread>

namespace pime::cap {

namespace {
constexpr size_t kMaxFrames = 8;
}

pime_status AudioStreams::start_capture(uint64_t session_id,
                                        const PcmFormat& format,
                                        uint32_t* stream_id) {
    if (!stream_id || !pcm_format_valid(format)) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    Stream& s = capture_[session_id];
    if (s.active) return PIME_BUSY;
    s.active = true;
    s.format = format;
    s.stream_id = next_stream_id_++;
    if (next_stream_id_ == 0) next_stream_id_ = 1;
    *stream_id = s.stream_id;
    return PIME_OK;
}

pime_status AudioStreams::stop_capture(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = capture_.find(session_id);
    if (it == capture_.end() || !it->second.active) return PIME_NOT_FOUND;
    it->second.active = false;
    it->second.queue.clear();
    return PIME_OK;
}

pime_status AudioStreams::push_capture(uint64_t session_id,
                                       const AudioFrameData& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = capture_.find(session_id);
    if (it == capture_.end() || !it->second.active) return PIME_NOT_FOUND;
    if (frame.data.size() > 16 * 1024) return PIME_BUFFER_TOO_SMALL;
    if (it->second.queue.size() >= kMaxFrames) it->second.queue.pop_front();
    it->second.queue.push_back(frame);
    return PIME_OK;
}

pime_status AudioStreams::read_capture(uint64_t session_id, AudioFrameData* frame,
                                       uint32_t timeout_ms) {
    if (!frame) return PIME_INVALID;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        auto it = capture_.find(session_id);
        if (it == capture_.end() || !it->second.active) return PIME_CLOSED;
        if (!it->second.queue.empty()) {
            *frame = std::move(it->second.queue.front());
            it->second.queue.pop_front();
            return PIME_OK;
        }
        if (timeout_ms == 0) return PIME_EMPTY;
        if (std::chrono::steady_clock::now() >= deadline) return PIME_TIMEOUT;
        /* Coarse poll avoids a second condition variable; capture is not hot. */
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        lock.lock();
    }
}

pime_status AudioStreams::start_playback(uint64_t session_id,
                                         const PcmFormat& format,
                                         uint32_t* stream_id) {
    if (!stream_id || !pcm_format_valid(format)) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(mutex_);
    Stream& s = playback_[session_id];
    if (s.active) return PIME_BUSY;
    s.active = true;
    s.format = format;
    s.stream_id = next_stream_id_++;
    if (next_stream_id_ == 0) next_stream_id_ = 1;
    *stream_id = s.stream_id;
    return PIME_OK;
}

pime_status AudioStreams::stop_playback(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(session_id);
    if (it == playback_.end() || !it->second.active) return PIME_NOT_FOUND;
    it->second.active = false;
    it->second.queue.clear();
    for (auto fifo = playback_fifo_.begin(); fifo != playback_fifo_.end();) {
        if (fifo->first == session_id) fifo = playback_fifo_.erase(fifo);
        else ++fifo;
    }
    return PIME_OK;
}

pime_status AudioStreams::push_playback(uint64_t session_id,
                                        const AudioFrameData& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(session_id);
    if (it == playback_.end() || !it->second.active) return PIME_NOT_FOUND;
    if (playback_fifo_.size() >= kMaxFrames * 4) return PIME_FULL;
    playback_fifo_.emplace_back(session_id, frame);
    return PIME_OK;
}

bool AudioStreams::pop_playback(uint64_t* session_id, AudioFrameData* frame) {
    if (!session_id || !frame) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (playback_fifo_.empty()) return false;
    *session_id = playback_fifo_.front().first;
    *frame = std::move(playback_fifo_.front().second);
    playback_fifo_.pop_front();
    return true;
}

size_t AudioStreams::playback_depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playback_fifo_.size();
}

bool AudioStreams::capture_active(uint64_t session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = capture_.find(session_id);
    return it != capture_.end() && it->second.active;
}

bool AudioStreams::playback_active(uint64_t session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(session_id);
    return it != playback_.end() && it->second.active;
}

void AudioStreams::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    capture_.clear();
    playback_.clear();
    playback_fifo_.clear();
}

}  // namespace pime::cap
