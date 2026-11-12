#ifndef PIME_CAPABILITIES_AUDIO_OPUS_HPP
#define PIME_CAPABILITIES_AUDIO_OPUS_HPP

#include <pime/abi.h>

#include <cstdint>
#include <vector>

#include "pcm.hpp"

namespace pime::cap {

bool opus_available();

/* Encodes exactly one 20 ms frame; returns RFC 6716 packets. */
pime_status opus_encode(const uint8_t* pcm, uint32_t len, const PcmFormat& format,
                        uint32_t bitrate, bool fec,
                        std::vector<std::vector<uint8_t>>* packets);

/* Decodes one packet to s16le at the requested rate/channels. */
pime_status opus_decode(const uint8_t* packet, uint32_t len, uint32_t rate,
                        uint32_t channels, bool plc, std::vector<uint8_t>* pcm);

}  // namespace pime::cap

#endif
