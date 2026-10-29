#include "OMTAudio.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>

static unsigned cases = 0;
static void check(const uint8_t* p, int channels, int samples) {
    const size_t plane = size_t(samples)*4;
    std::vector<uint8_t> expected;
    uint32_t mask = 0;
    for(int c=0;c<channels;++c) {
        bool used=false;
        for(size_t j=0;j<plane;++j) used |= p[c*plane+j]!=0;
        if(used) {mask |= uint32_t(1)<<c;expected.insert(expected.end(),p+c*plane,p+(c+1)*plane);}
    }
    // Reuse a dirty output to exercise both shrinking and growth.
    static std::vector<uint8_t> actual(12345,0x5a);
    uint32_t active=0xdeadbeef;
    if(!omt::encodeAudio(p,int(plane*channels),channels,samples,actual,active) || active!=mask || actual!=expected)
        throw std::runtime_error("byte oracle mismatch");
    omt::AudioExtHeader h{omt::Codec::FPA1,48000,samples,channels,active,0};
    std::vector<uint8_t> decoded;
    if(!omt::decodeAudio(h,actual.data(),actual.size(),decoded) || decoded.size()!=plane*channels || std::memcmp(decoded.data(),p,decoded.size()))
        throw std::runtime_error("roundtrip mismatch");
    ++cases;
}
int main(int argc,char**) try {
    if(argc>1) {
        volatile uint64_t checksum=0;
        for(int channels: {2,16,32}) for(int mode=0;mode<4;++mode) {
            const int samples=1025;
            std::vector<uint8_t> data(size_t(channels)*samples*4,0),output;
            if(mode) for(int c=0;c<channels;++c) if(mode!=1 || c%2==0)
                data[size_t(c)*samples*4+(mode==3?samples*4-1:0)]=1;
            uint32_t active=0;
            for(int i=0;i<100;++i) omt::encodeAudio(data.data(),int(data.size()),channels,samples,output,active);
            auto start=std::chrono::steady_clock::now();
            for(int i=0;i<10000;++i) {
                omt::encodeAudio(data.data(),int(data.size()),channels,samples,output,active);
                checksum=checksum+active+output.size();
            }
            double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/10000;
            std::printf("channels=%d mode=%d ns/frame=%.1f\n",channels,mode,ns);
        }
        std::printf("checksum=%llu\n",(unsigned long long)checksum);return 0;
    }
    for(int offset=0;offset<16;++offset) for(int samples: {1,2,3,7,8,9,31,32,33}) for(int channels: {1,2,3,16,32}) {
        size_t size=size_t(samples)*channels*4;
        std::vector<uint8_t> data(size+offset,0);auto* p=data.data()+offset;
        check(p,channels,samples);
        // Every byte position, including 4-byte tails and channel 31.
        for(size_t j=0;j<size;++j) {p[j]=0x80;check(p,channels,samples);p[j]=0;}
    }
    std::mt19937 rng(0x57ea);
    for(int i=0;i<2000;++i) {
        int channels=1+rng()%32,samples=1+rng()%2049,offset=rng()%16;
        std::vector<uint8_t> data(size_t(channels)*samples*4+offset,0);
        auto* p=data.data()+offset;
        for(int c=0;c<channels;++c) if(rng()%2) for(int j=0;j<samples*4;++j) p[size_t(c)*samples*4+j]=uint8_t(rng());
        check(p,channels,samples);
    }
    for(int channels:{1,32}) {
        int samples=1024*1024/(channels*4);
        std::vector<uint8_t> data(size_t(channels)*samples*4,0);data.back()=1;check(data.data(),channels,samples);
    }
    uint32_t active=123;std::vector<uint8_t> out{1,2,3};uint8_t data[4]{};
    if(omt::encodeAudio(nullptr,4,1,1,out,active) || omt::encodeAudio(data,3,1,1,out,active) ||
       omt::encodeAudio(data,-1,1,1,out,active) || omt::encodeAudio(data,4,33,1,out,active) ||
       omt::encodeAudio(data,4,1,0,out,active) || omt::encodeAudio(data,4,32,0x7fffffff,out,active))
        throw std::runtime_error("invalid input accepted");
    std::printf("PASS %u byte-oracle and roundtrip cases + invalid inputs\n",cases);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
