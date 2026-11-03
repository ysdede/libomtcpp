// Copyright (c) 2026 Zero Density. Licensed under the MIT License.
#pragma once
#include "OMTProtocol.h"
#include "OMTConstants.h"
#include <cstddef>
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #include <emmintrin.h>
    #define OMT_HAS_SSE2 1
  #endif
#elif defined(__ARM_NEON) || defined(__aarch64__)
  #include <arm_neon.h>
  #define OMT_HAS_NEON 1
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

// Fast non-zero plane detector using 64-byte chunks with SIMD (SSE2/NEON) and
// unrolled 64-bit scalar words, with byte-exact tail handling.
inline bool isPlaneUsed(const uint8_t* p, size_t plane) {
    size_t offset = 0;

#if defined(OMT_HAS_SSE2)
    while (offset + 64 <= plane) {
        __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + offset));
        __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + offset + 16));
        __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + offset + 32));
        __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + offset + 48));
        __m128i combined = _mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(combined, _mm_setzero_si128())) != 0xFFFF) {
            return true;
        }
        offset += 64;
    }
    while (offset + 16 <= plane) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + offset));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_setzero_si128())) != 0xFFFF) {
            return true;
        }
        offset += 16;
    }
#elif defined(OMT_HAS_NEON)
    while (offset + 64 <= plane) {
        uint8x16_t v0 = vld1q_u8(p + offset);
        uint8x16_t v1 = vld1q_u8(p + offset + 16);
        uint8x16_t v2 = vld1q_u8(p + offset + 32);
        uint8x16_t v3 = vld1q_u8(p + offset + 48);
        uint8x16_t combined = vorrq_u8(vorrq_u8(v0, v1), vorrq_u8(v2, v3));
        uint64x2_t c64 = vreinterpretq_u64_u8(combined);
        if ((vgetq_lane_u64(c64, 0) | vgetq_lane_u64(c64, 1)) != 0) {
            return true;
        }
        offset += 64;
    }
    while (offset + 16 <= plane) {
        uint8x16_t v = vld1q_u8(p + offset);
        uint64x2_t c64 = vreinterpretq_u64_u8(v);
        if ((vgetq_lane_u64(c64, 0) | vgetq_lane_u64(c64, 1)) != 0) {
            return true;
        }
        offset += 16;
    }
#else
    while (offset + 64 <= plane) {
        uint64_t w0, w1, w2, w3, w4, w5, w6, w7;
        std::memcpy(&w0, p + offset, 8);
        std::memcpy(&w1, p + offset + 8, 8);
        std::memcpy(&w2, p + offset + 16, 8);
        std::memcpy(&w3, p + offset + 24, 8);
        std::memcpy(&w4, p + offset + 32, 8);
        std::memcpy(&w5, p + offset + 40, 8);
        std::memcpy(&w6, p + offset + 48, 8);
        std::memcpy(&w7, p + offset + 56, 8);
        if (((w0 | w1) | (w2 | w3)) | ((w4 | w5) | (w6 | w7))) {
            return true;
        }
        offset += 64;
    }
#endif

    while (offset + sizeof(uint64_t) <= plane) {
        uint64_t val;
        std::memcpy(&val, p + offset, sizeof(uint64_t));
        if (val) return true;
        offset += sizeof(uint64_t);
    }
    while (offset < plane) {
        if (p[offset]) return true;
        offset += 1;
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
        if(isPlaneUsed(p, plane)) {
            active|=uint32_t{1}<<ch;
            output.insert(output.end(),p,p+plane);
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
