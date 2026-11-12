#ifndef PIME_CAPABILITIES_AUDIO_STREAM_HPP
#define PIME_CAPABILITIES_AUDIO_STREAM_HPP

#include <pime/abi.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "pcm.hpp"

namespace pime::cap {

struct AudioFrameData {
    PcmFormat format;
    std::vector<uint8_t> data;
    uint64_t time_ms = 0;
};

/* One capture and one playback stream per session. Rings are bounded. */
class AudioStreams {
public:
    pime_status start_capture(uint64_t session_id, const PcmFormat& format,
                              uint32_t* stream_id);
    pime_status stop_capture(uint64_t session_id);
    pime_status push_capture(uint64_t session_id, const AudioFrameData& frame);
    pime_status read_capture(uint64_t session_id, AudioFrameData* frame,
                             uint32_t timeout_ms);

    pime_status start_playback(uint64_t session_id, const PcmFormat& format,
                               uint32_t* stream_id);
    pime_status stop_playback(uint64_t session_id);
    pime_status push_playback(uint64_t session_id, const AudioFrameData& frame);
    bool pop_playback(uint64_t* session_id, AudioFrameData* frame);
    size_t playback_depth() const;

    bool capture_active(uint64_t session_id) const;
    bool playback_active(uint64_t session_id) const;
    void stop_all();

private:
    struct Stream {
        uint32_t stream_id = 0;
        PcmFormat format;
        bool active = false;
        std::deque<AudioFrameData> queue;
    };

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Stream> capture_;
    std::unordered_map<uint64_t, Stream> playback_;
    std::deque<std::pair<uint64_t, AudioFrameData>> playback_fifo_;
    uint32_t next_stream_id_ = 1;
};

}  // namespace pime::cap

#endif
