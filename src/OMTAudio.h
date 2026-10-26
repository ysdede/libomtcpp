// Copyright (c) 2026 Zero Density. Licensed under the MIT License.
#pragma once
#include "OMTProtocol.h"
#include "OMTConstants.h"
#include <cstddef>
#include <cstring>
#include <vector>

namespace omt {
static_assert(sizeof(AudioExtHeader)==24, "FPA1 wire header is 24 bytes");
static_assert(offsetof(AudioExtHeader,samplesPerChannel)==8 &&
              offsetof(AudioExtHeader,channels)==12 &&
              offsetof(AudioExtHeader,activeChannels)==16, "FPA1 field order");

inline bool audioSize(int channels, int samples, size_t& bytes) {
    if(channels<1 || channels>32 || samples<1)return false;
    const uint64_t size=static_cast<uint64_t>(channels)*static_cast<uint64_t>(samples)*4;
    if(size>static_cast<uint64_t>(Constants::AUDIO_MAX_SIZE))return false;
    bytes=static_cast<size_t>(size);return true;
}

// Silence is an all-zero byte plane, matching libomtnet. Negative zero, NaN,
// infinities and subnormal samples retain their original bit representation.
inline bool isPlaneActive(const uint8_t* p, size_t plane) {
    size_t i = 0;
    for (; i + 8 <= plane; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, sizeof(w));
        if (w != 0) return true;
    }
    for (; i + 4 <= plane; i += 4) {
        uint32_t w;
        std::memcpy(&w, p + i, sizeof(w));
        if (w != 0) return true;
    }
    return false;
}

inline bool encodeAudio(const void* data, int length, int channels, int samples,
                        std::vector<uint8_t>& output, uint32_t& active) {
    size_t bytes=0;
    if(!data || length<0 || !audioSize(channels,samples,bytes) ||
       static_cast<size_t>(length)<bytes)return false;
    const size_t plane=static_cast<size_t>(samples)*4;
    const auto* input=static_cast<const uint8_t*>(data);

    uint32_t activeMask = 0;
    for(int ch=0;ch<channels;++ch) {
        if (isPlaneActive(input + ch * plane, plane)) {
            activeMask |= (uint32_t{1} << ch);
        }
    }
    active = activeMask;

    int activeCount = 0;
    uint32_t temp = activeMask;
    while (temp) {
        activeCount += (temp & 1);
        temp >>= 1;
    }

    const size_t activeBytes = static_cast<size_t>(activeCount) * plane;
    output.resize(activeBytes);
    uint8_t* dst = output.data();
    for(int ch=0;ch<channels;++ch) {
        if (activeMask & (uint32_t{1} << ch)) {
            std::memcpy(dst, input + ch * plane, plane);
            dst += plane;
        }
    }
    return true;
}

inline bool decodeAudio(const AudioExtHeader& header, const uint8_t* data, size_t length,
                        std::vector<uint8_t>& output) {
    size_t bytes=0;
    if(header.codec!=Codec::FPA1 || header.sampleRate<=0 ||
       !audioSize(header.channels,header.samplesPerChannel,bytes))return false;
    const uint32_t allowed=header.channels==32 ? UINT32_MAX : (uint32_t{1}<<header.channels)-1;
    if(header.activeChannels & ~allowed)return false;
    unsigned count=0;uint32_t bits=header.activeChannels;
    while(bits){count+=bits&1;bits>>=1;}
    const size_t plane=static_cast<size_t>(header.samplesPerChannel)*4;
    if(length!=count*plane || (length && !data))return false;
    output.assign(bytes,0);
    size_t offset=0;
    for(int ch=0;ch<header.channels;++ch)if(header.activeChannels&(uint32_t{1}<<ch)){
        std::memcpy(output.data()+ch*plane,data+offset,plane);offset+=plane;
    }
    return true;
}
}
