#include "pcm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pime::cap {

bool pcm_format_valid(const PcmFormat& format) {
    if (format.format != PIME_AUDIO_S16LE && format.format != PIME_AUDIO_F32LE)
        return false;
    if (format.channels == 0 || format.channels > 8) return false;
    if (format.rate < 8000 || format.rate > 192000) return false;
    return true;
}

uint32_t pcm_bytes_per_sample(uint32_t format) {
    return format == PIME_AUDIO_F32LE ? 4u : 2u;
}

namespace {

float read_sample(const uint8_t* p, uint32_t format, uint32_t index) {
    if (format == PIME_AUDIO_F32LE) {
        float v;
        std::memcpy(&v, p + index * 4u, 4);
        return v;
    }
    int16_t v;
    std::memcpy(&v, p + index * 2u, 2);
    return static_cast<float>(v) / 32768.0f;
}

void write_sample(uint8_t* p, uint32_t format, uint32_t index, float v) {
    if (format == PIME_AUDIO_F32LE) {
        std::memcpy(p + index * 4u, &v, 4);
    } else {
        float clamped = std::max(-1.0f, std::min(1.0f, v));
        int16_t s = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        std::memcpy(p + index * 2u, &s, 2);
    }
}

}  // namespace

pime_status pcm_convert(const uint8_t* src, uint32_t src_len,
                        const PcmFormat& from, const PcmFormat& to, float gain,
                        std::vector<uint8_t>* out) {
    if (!out || (!src && src_len != 0)) return PIME_INVALID;
    if (!pcm_format_valid(from) || !pcm_format_valid(to)) return PIME_INVALID;
    const uint32_t in_bps = pcm_bytes_per_sample(from.format);
    const uint32_t out_bps = pcm_bytes_per_sample(to.format);
    const uint32_t in_frame = in_bps * from.channels;
    const uint32_t out_frame = out_bps * to.channels;
    if (src_len % in_frame != 0) return PIME_INVALID;
    const uint32_t in_frames = src_len / in_frame;
    const uint64_t out_frames64 =
        (static_cast<uint64_t>(in_frames) * to.rate + from.rate / 2) / from.rate;
    const uint32_t out_frames = static_cast<uint32_t>(out_frames64);
    out->resize(static_cast<size_t>(out_frames) * out_frame);
    if (out_frames == 0) return PIME_OK;

    std::vector<float> scratch(static_cast<size_t>(from.channels) * out_frames);
    for (uint32_t ch = 0; ch < from.channels; ++ch) {
        for (uint32_t i = 0; i < out_frames; ++i) {
            const double pos =
                static_cast<double>(i) * from.rate / static_cast<double>(to.rate);
            uint32_t i0 = static_cast<uint32_t>(pos);
            if (i0 >= in_frames) i0 = in_frames - 1;
            uint32_t i1 = i0 + 1 < in_frames ? i0 + 1 : i0;
            const float t = static_cast<float>(pos - i0);
            float s0 = read_sample(src, from.format, i0 * from.channels + ch);
            float s1 = read_sample(src, from.format, i1 * from.channels + ch);
            scratch[static_cast<size_t>(ch) * out_frames + i] =
                s0 + (s1 - s0) * t;
        }
    }

    for (uint32_t i = 0; i < out_frames; ++i) {
        for (uint32_t ch = 0; ch < to.channels; ++ch) {
            float value;
            if (to.channels == from.channels) {
                value = scratch[static_cast<size_t>(ch) * out_frames + i];
            } else if (to.channels < from.channels) {
                /* Downmix: keep the first channel group's average. */
                float sum = 0.0f;
                for (uint32_t c = 0; c < from.channels; ++c)
                    sum += scratch[static_cast<size_t>(c) * out_frames + i];
                value = sum / static_cast<float>(from.channels);
            } else {
                /* Upmix: repeat available channels. */
                value = scratch[static_cast<size_t>(ch % from.channels) * out_frames + i];
            }
            value *= gain;
            write_sample(out->data(), to.format,
                         i * to.channels + ch, value);
        }
    }
    return PIME_OK;
}

}  // namespace pime::cap
