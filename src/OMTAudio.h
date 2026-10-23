// Copyright (c) 2026 Zero Density. Licensed under the MIT License.
#pragma once
#include "OMTProtocol.h"
#include "OMTConstants.h"
#include <cstddef>
#include <cstring>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

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

inline bool isPlaneActive(const uint8_t* p, size_t plane) {
    size_t offset = 0;
    while (offset + sizeof(uint64_t) <= plane) {
        uint64_t word;
        std::memcpy(&word, p + offset, sizeof(uint64_t));
        if (word != 0) return true;
        offset += sizeof(uint64_t);
    }
    while (offset < plane) {
        if (p[offset] != 0) return true;
        ++offset;
    }
    return false;
}

// Silence is an all-zero byte plane, matching libomtnet. Negative zero, NaN,
// infinities and subnormal samples retain their original bit representation.
inline bool encodeAudio(const void* data, int length, int channels, int samples,
                        std::vector<uint8_t>& output, uint32_t& active) {
    size_t bytes=0;
    if(!data || length<0 || !audioSize(channels,samples,bytes) ||
       static_cast<size_t>(length)<bytes)return false;
    const size_t plane=static_cast<size_t>(samples)*4;
    const auto* input=static_cast<const uint8_t*>(data);
    output.clear();output.reserve(bytes);active=0;
    for(int ch=0;ch<channels;++ch) {
        const uint8_t* p=input+ch*plane;
        if(isPlaneActive(p, plane)) {
            active|=uint32_t{1}<<ch;
            const size_t oldSize = output.size();
            output.resize(oldSize + plane);
            std::memcpy(output.data() + oldSize, p, plane);
        }
    }
    return true;
}

inline unsigned popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_popcount(v));
#elif defined(_MSC_VER)
    return static_cast<unsigned>(__popcnt(v));
#else
    v = v - ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    return (((v + (v >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
#endif
}

inline bool decodeAudio(const AudioExtHeader& header, const uint8_t* data, size_t length,
                        std::vector<uint8_t>& output) {
    size_t bytes=0;
    if(header.codec!=Codec::FPA1 || header.sampleRate<=0 ||
       !audioSize(header.channels,header.samplesPerChannel,bytes))return false;
    const uint32_t allowed=header.channels==32 ? UINT32_MAX : (uint32_t{1}<<header.channels)-1;
    if(header.activeChannels & ~allowed)return false;
    unsigned count = popcount32(header.activeChannels);
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
