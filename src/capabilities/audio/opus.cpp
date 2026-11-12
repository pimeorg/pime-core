#include "opus.hpp"

#include <cstring>

#if defined(PIME_HAVE_OPUS)
#include <opus.h>
#endif

namespace pime::cap {

bool opus_available() {
#if defined(PIME_HAVE_OPUS)
    return true;
#else
    return false;
#endif
}

pime_status opus_encode(const uint8_t* pcm, uint32_t len, const PcmFormat& format,
                        uint32_t bitrate, bool fec,
                        std::vector<std::vector<uint8_t>>* packets) {
    if (!packets || !pcm) return PIME_INVALID;
#if defined(PIME_HAVE_OPUS)
    if (!pcm_format_valid(format)) return PIME_INVALID;
    const uint32_t frame_samples = format.rate / 50; /* 20 ms */
    const uint32_t bytes_per_sample = pcm_bytes_per_sample(format.format);
    const uint32_t frame_bytes = frame_samples * format.channels * bytes_per_sample;
    if (len == 0 || len % frame_bytes != 0) return PIME_INVALID;

    int error = 0;
    OpusEncoder* encoder =
        opus_encoder_create(static_cast<opus_int32>(format.rate),
                            static_cast<int>(format.channels),
                            OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error != OPUS_OK) {
        if (encoder) opus_encoder_destroy(encoder);
        return PIME_RESOURCE;
    }
    if (bitrate > 0)
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(fec ? 1 : 0));

    packets->clear();
    pime_status status = PIME_OK;
    const uint32_t frames = len / frame_bytes;
    for (uint32_t i = 0; i < frames; ++i) {
        std::vector<uint8_t> packet(1280);
        const opus_int32 produced = opus_encode(
            encoder,
            reinterpret_cast<const opus_int16*>(pcm + static_cast<size_t>(i) * frame_bytes),
            static_cast<int>(frame_samples), packet.data(),
            static_cast<opus_int32>(packet.size()));
        if (produced < 0) {
            status = PIME_RESOURCE;
            break;
        }
        packet.resize(static_cast<size_t>(produced));
        packets->push_back(std::move(packet));
    }
    opus_encoder_destroy(encoder);
    return status;
#else
    (void)len; (void)format; (void)bitrate; (void)fec;
    return PIME_UNSUPPORTED;
#endif
}

pime_status opus_decode(const uint8_t* packet, uint32_t len, uint32_t rate,
                        uint32_t channels, bool plc, std::vector<uint8_t>* pcm) {
    if (!pcm || (!packet && len != 0)) return PIME_INVALID;
#if defined(PIME_HAVE_OPUS)
    if (channels == 0 || channels > 2) return PIME_INVALID;
    int error = 0;
    OpusDecoder* decoder =
        opus_decoder_create(static_cast<opus_int32>(rate), static_cast<int>(channels),
                            &error);
    if (!decoder || error != OPUS_OK) {
        if (decoder) opus_decoder_destroy(decoder);
        return PIME_RESOURCE;
    }
    const uint32_t max_samples = 5760;
    std::vector<opus_int16> samples(static_cast<size_t>(max_samples) * channels);
    int produced = opus_decode(
        decoder, plc ? nullptr : packet, plc ? 0 : static_cast<opus_int32>(len),
        samples.data(), static_cast<int>(max_samples), plc ? 1 : 0);
    opus_decoder_destroy(decoder);
    if (produced < 0) return PIME_RESOURCE;
    const size_t bytes = static_cast<size_t>(produced) * channels * sizeof(opus_int16);
    pcm->resize(bytes);
    std::memcpy(pcm->data(), samples.data(), bytes);
    return PIME_OK;
#else
    (void)packet; (void)len; (void)rate; (void)channels; (void)plc;
    return PIME_UNSUPPORTED;
#endif
}

}  // namespace pime::cap
