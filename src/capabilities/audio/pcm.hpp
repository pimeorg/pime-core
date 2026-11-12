#ifndef PIME_CAPABILITIES_AUDIO_PCM_HPP
#define PIME_CAPABILITIES_AUDIO_PCM_HPP

#include <pime/hal.h>

#include <cstdint>
#include <vector>

namespace pime::cap {

struct PcmFormat {
    uint32_t rate = 16000;
    uint32_t channels = 1;
    uint32_t format = PIME_AUDIO_S16LE;
};

bool pcm_format_valid(const PcmFormat& format);
uint32_t pcm_bytes_per_sample(uint32_t format);

/* Converts format/rate/channels and applies linear gain. */
pime_status pcm_convert(const uint8_t* src, uint32_t src_len,
                        const PcmFormat& from, const PcmFormat& to,
                        float gain, std::vector<uint8_t>* out);

}  // namespace pime::cap

#endif
