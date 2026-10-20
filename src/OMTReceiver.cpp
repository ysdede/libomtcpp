// Copyright (c) 2026 Zero Density. Licensed under the MIT License.
// OMT receiver: socket owners enqueue bounded compressed frames; callers decode.
// Protocol behavior follows libomtnet. No dependency on a host application.
#include "OMTReceiver.h"
#include "OMTDiscovery.h"
#include "OMTAudio.h"
#include "OMTLogging.h"
#include "vmxcodec.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#else
#include "omt_os_win.h"
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace omt {
namespace {
using Clock = std::chrono::steady_clock;
using Socket = decltype(socket(AF_INET,SOCK_STREAM,0));
constexpr Socket invalidSocket=static_cast<Socket>(-1);
int indexOf(int type) { return type==2 ? 0 : type==4 ? 1 : type==1 ? 2 : -1; }
int typeOf(int index) { return index==0 ? 2 : index==1 ? 4 : 1; }
constexpr size_t limits[]{4,10,60};
bool validMetadata(const void* p, int n) {
    return p && n>0 && n<=65535 && static_cast<const char*>(p)[n-1]=='\0' &&
           std::memchr(p,0,static_cast<size_t>(n-1))==nullptr;
}
std::string infoAttribute(const std::string& xml,const std::string& name) {
    size_t start=0;
    while((start=xml.find(name,start))!=std::string::npos) {
        if(start && xml[start-1]!=' ' && xml[start-1]!='\n' && xml[start-1]!='\t'){start+=name.size();continue;}
        size_t p=xml.find_first_not_of(" \t\r\n",start+name.size());
        if(p==std::string::npos || xml[p]!='='){start+=name.size();continue;}
        p=xml.find_first_not_of(" \t\r\n",p+1);
        if(p==std::string::npos || (xml[p]!='\'' && xml[p]!='"'))return {};
        size_t end=xml.find(xml[p],p+1);if(end==std::string::npos)return {};
        std::string result=xml.substr(p+1,end-p-1);
        const std::pair<const char*,const char*> entities[]{
            {"&quot;","\""},{"&apos;","'"},{"&lt;","<"},{"&gt;",">"},{"&amp;","&"}};
        for(const auto& entity:entities) {
            size_t offset=0;
            while((offset=result.find(entity.first,offset))!=std::string::npos) {
                result.replace(offset,std::strlen(entity.first),entity.second);offset+=std::strlen(entity.second);
            }
        }
        return result;
    }
    return {};
}
// Reject truncated full frames before VMX_LoadFrom: a DC-only payload is legal
// for preview, but loading it as full video would reuse the previous AC planes.
bool validVmxPayload(const uint8_t* bytes,size_t length,int height,bool preview) {
    if(length<5)return false;
    const size_t prefix=bytes[0]==VMX_CODEC_FORMAT_EXTENDED?5:3;
    const int slices=(height+15)/16;
    if(bytes[prefix-1]!=(slices&255))return false;
    size_t offset=prefix;
    for(int part=0;part<2;++part) {
        if(part==1 && offset==length)return preview;
        for(int i=0;i<slices;++i) {
            if(length-offset<4)return false;
            uint32_t n=0;std::memcpy(&n,bytes+offset,4);offset+=4;
            if(n>length-offset)return false;
            offset+=n;
        }
    }
    return offset==length;
}
void copyText(char* dst,size_t capacity,const std::string& value) {
    size_t n=(std::min)(capacity-1,value.size());std::memcpy(dst,value.data(),n);dst[n]=0;
}
}

struct Receiver::Impl {
    struct Address { sockaddr_storage value{}; socklen_t size=0; int family=0; };
    struct Packet { FrameHeader header{}; std::vector<uint8_t> body; uint64_t order=0; };
    struct Output {
        std::mutex mutex;
        OMTMediaFrame frame{};
        std::vector<uint8_t> pixels,compressed,metadata;
    };
    std::string address;
    int types;
    OMTPreferredVideoFormat format;
    std::vector<Address> addresses;
    std::atomic<bool> running{false};
    std::array<std::thread,2> workers;
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::deque<Packet>,3> queues;
    std::array<Output,3> outputs;
    std::array<OMTStatistics,3> stats{};
    std::array<OMTStatistics,3> previous{};
    uint64_t order=0,settingsVersion=1;
    int flags;
    OMTQuality quality=OMTQuality_Default;
    OMTTally localTally{}, remoteTally{};
    bool haveTally=false,tallyChanged=false;
    OMTSenderInfo info{};
    std::array<bool,2> connected{};
    std::deque<std::vector<uint8_t>> outgoing;
    VMX_INSTANCE* decoder=nullptr;
    int decoderW=0,decoderH=0,decoderColor=0;

    Impl(const char* a,int t,OMTPreferredVideoFormat f,int fl):address(a?a:""),types(t),format(f),flags(fl){}
    ~Impl() {
        running=false;cv.notify_all();
        for(auto& thread:workers)if(thread.joinable())thread.join();
        if(decoder)VMX_Destroy(decoder);
    }
    bool resolve() {
        std::string host,port;
        if(address.rfind("omt://",0)==0) {
            std::string endpoint=address.substr(6);
            if(endpoint.empty() || endpoint.find('/')!=std::string::npos)return false;
            if(endpoint[0]=='[') {
                auto end=endpoint.find(']');
                if(end==std::string::npos || end+1>=endpoint.size() || endpoint[end+1]!=':')return false;
                host=endpoint.substr(1,end-1);port=endpoint.substr(end+2);
            } else {
                auto separator=endpoint.rfind(':');
                if(separator==std::string::npos)return false;
                host=endpoint.substr(0,separator);port=endpoint.substr(separator+1);
            }
        } else {
            // Copy under the discovery cache lock; never retain findEntry's pointer.
            for(const auto& e:Discovery::getInstance().getEntries())
                if(e.name==address || e.fullName==address) {
                    host=e.hostAddress;port=std::to_string(e.port);break;
                }
        }
        if(host.empty() || port.empty() || port.size()>5 ||
           port.find_first_not_of("0123456789")!=std::string::npos)return false;
        int number=std::stoi(port);if(number<1 || number>65535)return false;
        addrinfo hints{};hints.ai_socktype=SOCK_STREAM;hints.ai_protocol=IPPROTO_TCP;
        hints.ai_family=AF_UNSPEC;hints.ai_flags=AI_NUMERICSERV;
        addrinfo* list=nullptr;
        // Resolve before starting owned workers: shutdown never waits on DNS.
        if(getaddrinfo(host.c_str(),port.c_str(),&hints,&list))return false;
        std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owner(list,freeaddrinfo);
        for(auto* a=list;a && addresses.size()<16;a=a->ai_next)if(a->ai_addrlen<=sizeof(sockaddr_storage)) {
            Address entry;entry.family=a->ai_family;entry.size=static_cast<socklen_t>(a->ai_addrlen);
            std::memcpy(&entry.value,a->ai_addr,a->ai_addrlen);addresses.push_back(entry);
        }
        return !addresses.empty();
    }
    Socket connectSocket() {
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        for(const auto& a:addresses) {
            if(!running || Clock::now()>=deadline)break;
            Socket fd=socket(a.family,SOCK_STREAM,IPPROTO_TCP);
            if(fd==invalidSocket)continue;
            int fl=fcntl(fd,F_GETFL,0);
            if(fl<0 || fcntl(fd,F_SETFL,fl|O_NONBLOCK)){close(fd);continue;}
            int one=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
            setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
#ifdef SO_NOSIGPIPE
            setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
            int receiveBytes=Constants::NETWORK_RECEIVE_BUFFER;
            setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&receiveBytes,sizeof(receiveBytes));
            int result=connect(fd,reinterpret_cast<const sockaddr*>(&a.value),a.size);
            if(result==0)return fd;
            if(errno==EINPROGRESS || errno==EWOULDBLOCK) {
                const auto attemptEnd=(std::min)(deadline,Clock::now()+std::chrono::milliseconds(500));
                while(running && Clock::now()<attemptEnd) {
                    pollfd p{};p.fd=fd;p.events=POLLOUT;
                    int ready=poll(&p,1,20);
                    if(ready<0 && errno==EINTR)continue;
                    if(ready<0)break;
                    if(!ready)continue;
                    int error=0;socklen_t size=sizeof(error);
                    if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)==0 && error==0)return fd;
                    break;
                }
            }
            close(fd);
        }
        return invalidSocket;
    }
    bool write(Socket fd,const std::vector<uint8_t>& bytes) {
        size_t offset=0;auto deadline=Clock::now()+std::chrono::milliseconds(500);
        while(running && offset<bytes.size() && Clock::now()<deadline) {
            int n=send(fd,reinterpret_cast<const char*>(bytes.data()+offset),
                       static_cast<int>(bytes.size()-offset),MSG_NOSIGNAL);
            if(n>0){offset+=n;continue;}
            if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) {
                pollfd p{};p.fd=fd;p.events=POLLOUT;poll(&p,1,20);continue;
            }
            return false;
        }
        // This worker closes/reconnects on an incomplete control write. No later
        // bytes are appended to a partial record on the same connection.
        return offset==bytes.size();
    }
    bool command(Socket fd,const char* xml) {
        size_t length=std::strlen(xml);
        FrameHeader h{1,1,0,0,static_cast<int32_t>(length)};
        std::vector<uint8_t> bytes(sizeof(h)+length);
        std::memcpy(bytes.data(),&h,sizeof(h));std::memcpy(bytes.data()+sizeof(h),xml,length);
        return write(fd,bytes);
    }
    bool settings(Socket fd,int worker,bool initial,uint64_t& version) {
        int fl;OMTQuality q;OMTTally tally;uint64_t latest;
        {
            std::lock_guard<std::mutex> lock(mutex);
            latest=settingsVersion;if(!initial && version==latest)return true;
            fl=flags;q=quality;tally=localTally;
        }
        using namespace MetadataConstants;
        if(worker==0 && (types&2) && !command(fd,(fl&OMTReceiveFlags_Preview)?CHANNEL_PREVIEW_VIDEO_ON:CHANNEL_PREVIEW_VIDEO_OFF))return false;
        const char* qualityName=q==OMTQuality_High?"High":q==OMTQuality_Medium?"Medium":q==OMTQuality_Low?"Low":"Default";
        std::string qualityXml=std::string("<OMTSettings Quality=\"")+qualityName+"\" />";
        if(worker==0 && (types&2) && !command(fd,qualityXml.c_str()))return false;
        if(initial) {
            if(worker==0 && (types&2) && !command(fd,CHANNEL_SUBSCRIBE_VIDEO))return false;
            if(worker==1 && !command(fd,CHANNEL_SUBSCRIBE_AUDIO))return false;
            if((worker==0 || !(types&3)) && !command(fd,CHANNEL_SUBSCRIBE_METADATA))return false;
        }
        const char* t=tally.preview?(tally.program?TALLY_PREVIEWPROGRAM_LEGACY:TALLY_PREVIEW_LEGACY):
                                   (tally.program?TALLY_PROGRAM_LEGACY:TALLY_NONE_LEGACY);
        if(!command(fd,t))return false;
        version=latest;return true;
    }
    bool control(const std::string& xml) {
        using namespace MetadataConstants;
        OMTTally tally{};bool recognized=true;
        if(xml==TALLY_PREVIEWPROGRAM_LEGACY || xml==TALLY_PREVIEWPROGRAM)tally={1,1};
        else if(xml==TALLY_PREVIEW_LEGACY || xml==TALLY_PREVIEW)tally={1,0};
        else if(xml==TALLY_PROGRAM_LEGACY || xml==TALLY_PROGRAM)tally={0,1};
        else if(xml!=TALLY_NONE_LEGACY && xml!=TALLY_NONE)recognized=false;
        std::lock_guard<std::mutex> lock(mutex);
        if(recognized) {
            if(!haveTally || tally.preview!=remoteTally.preview || tally.program!=remoteTally.program) {
                remoteTally=tally;haveTally=tallyChanged=true;cv.notify_all();
            }
        } else if(xml.rfind("<OMTInfo ",0)==0) {
            info={};
            copyText(info.ProductName,sizeof(info.ProductName),infoAttribute(xml,"ProductName"));
            copyText(info.Manufacturer,sizeof(info.Manufacturer),infoAttribute(xml,"Manufacturer"));
            copyText(info.Version,sizeof(info.Version),infoAttribute(xml,"Version"));
        }
        return recognized;
    }
    bool parse(std::vector<uint8_t>& bytes,int worker) {
        size_t offset=0;
        while(bytes.size()-offset>=sizeof(FrameHeader)) {
            FrameHeader h{};std::memcpy(&h,bytes.data()+offset,sizeof(h));
            const int index=indexOf(h.frameType);
            const int extended=index==0?32:index==1?24:0;
            const int maximum=index==0?Constants::VIDEO_MAX_SIZE+32+65535:
                              index==1?Constants::AUDIO_MAX_SIZE+24+65535:65535;
            if(h.version!=1 || index<0 || h.dataLength<extended || h.dataLength>maximum ||
               h.metadataLength>h.dataLength-extended)return false;
            size_t total=sizeof(h)+static_cast<size_t>(h.dataLength);
            if(bytes.size()-offset<total)break;
            const auto* body=bytes.data()+offset+sizeof(h);
            bool consumed=false;
            if(index==2) {
                // libomtnet uses metadataLength=0 and no NUL for standalone XML.
                // Accept the NUL-terminated form used by other OMT applications too.
                if(h.dataLength<=0 || (h.metadataLength && h.metadataLength!=h.dataLength))return false;
                size_t length=h.dataLength;
                if(body[length-1]==0)--length;
                if(std::memchr(body,0,length))return false;
                consumed=control(std::string(reinterpret_cast<const char*>(body),length));
            } else if(h.metadataLength && body[h.dataLength-1]!=0)return false;
            const bool correctWorker=index==1?worker==1:worker==0 || !(types&3);
            if(!consumed && correctWorker && (types&h.frameType)) {
                Packet packet;packet.header=h;packet.body.assign(body,body+h.dataLength);
                std::lock_guard<std::mutex> lock(mutex);
                packet.order=++order;
                auto& queue=queues[index];auto& stat=stats[index];
                stat.Frames++;stat.BytesReceived+=static_cast<int64_t>(total);
                if(queue.size()>=limits[index]){queue.pop_front();stat.FramesDropped++;}
                queue.push_back(std::move(packet));cv.notify_all();
            }
            offset+=total;
        }
        if(offset)bytes.erase(bytes.begin(),bytes.begin()+static_cast<ptrdiff_t>(offset));
        return bytes.size()<=static_cast<size_t>(Constants::VIDEO_MAX_SIZE+32+65535+sizeof(FrameHeader));
    }
    void connectionState(int worker,bool value) {
        std::lock_guard<std::mutex> lock(mutex);connected[worker]=value;
        if(!value) {
            for(int i=0;i<3;++i)if((i==1)==(worker==1) || (i==2 && !(types&3))) {
                stats[i].FramesDropped+=static_cast<int64_t>(queues[i].size());queues[i].clear();
            }
            if(worker==0 || !(types&3))outgoing.clear();
        }
        if(!connected[0] && !connected[1]) { info={};haveTally=false;tallyChanged=false;remoteTally={}; }
    }
    void run(int worker) noexcept {
        while(running) {
            Socket fd=invalidSocket;
            try {
                fd=connectSocket();
                if(fd!=invalidSocket) {
                    connectionState(worker,true);
                    uint64_t version=0;
                    bool ok=settings(fd,worker,true,version);
                    std::vector<uint8_t> bytes;bytes.reserve(1024*1024);
                    std::array<uint8_t,65536> chunk;
                    while(ok && running) {
                        ok=settings(fd,worker,false,version);if(!ok)break;
                        if(worker==0 || !(types&3)) {
                            std::vector<uint8_t> request;
                            {std::lock_guard<std::mutex> lock(mutex);
                             if(!outgoing.empty()){request=std::move(outgoing.front());outgoing.pop_front();}}
                            if(!request.empty() && !write(fd,request))break;
                        }
                        pollfd p{};p.fd=fd;p.events=POLLIN;
                        int ready=poll(&p,1,20);
                        if(ready<0){if(errno==EINTR)continue;break;}
                        if(!ready)continue;
                        int n=recv(fd,chunk.data(),static_cast<int>(chunk.size()),0);
                        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))continue;
                        if(n<=0)break;
                        bytes.insert(bytes.end(),chunk.begin(),chunk.begin()+n);
                        ok=parse(bytes,worker);
                    }
                }
            } catch(const std::exception& e) {LOGE("Receiver: %s",e.what());}
            if(fd!=invalidSocket){shutdown(fd,SHUT_RDWR);close(fd);}
            connectionState(worker,false);
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock,std::chrono::milliseconds(250),[&]{return !running.load();});
        }
    }
    bool decode(Packet& packet,int index,Output& output,int currentFlags);
};

bool Receiver::Impl::decode(Packet& p,int index,Output& out,int currentFlags) {
    auto& f=out.frame;f={};f.Type=static_cast<OMTFrameType>(p.header.frameType);f.Timestamp=p.header.timestamp;
    const size_t metaLength=p.header.metadataLength;
    if(index==2) {
        out.pixels=std::move(p.body);
        if(out.pixels.empty() || out.pixels.back()!=0)out.pixels.push_back(0);
        f.Data=out.pixels.data();f.DataLength=static_cast<int>(out.pixels.size());
        return true;
    }
    const size_t extended=index==0?32:24;
    const size_t payload=p.body.size()-extended-metaLength;
    out.metadata.assign(p.body.end()-static_cast<ptrdiff_t>(metaLength),p.body.end());
    f.FrameMetadata=metaLength?out.metadata.data():nullptr;f.FrameMetadataLength=static_cast<int>(metaLength);
    if(index==1) {
        AudioExtHeader header{};std::memcpy(&header,p.body.data(),24);
        if(!decodeAudio(header,p.body.data()+24,payload,out.pixels))return false;
        f.Codec=OMTCodec_FPA1;f.SampleRate=header.sampleRate;f.Channels=header.channels;f.SamplesPerChannel=header.samplesPerChannel;
    } else {
        VideoExtHeader header{};std::memcpy(&header,p.body.data(),32);
        if(header.codec!=Codec::VMX1 || header.width<16 || header.width>7680 || (header.width&1) ||
           header.height<16 || header.height>4320 || header.frameRateN<=0 || header.frameRateD<=0 ||
           (header.colorSpace!=0 && header.colorSpace!=601 && header.colorSpace!=709) ||
           payload<5 || payload>static_cast<size_t>(header.width)*header.height*4+4096)return false;
        bool alpha=(header.flags&VideoFlags::Alpha)!=0,preview=(header.flags&VideoFlags::Preview)!=0;
        bool interlaced=(header.flags&VideoFlags::Interlaced)!=0,high=(header.flags&VideoFlags::HighBitDepth)!=0;
        uint8_t* bits=p.body.data()+32;
        const int streamFormat=bits[0]==VMX_CODEC_FORMAT_EXTENDED?bits[2]:bits[0];
        if(streamFormat!=(interlaced?VMX_CODEC_FORMAT_INTERLACED:VMX_CODEC_FORMAT_PROGRESSIVE))return false;
        if(interlaced && header.height!=480 && header.height!=576 && header.height!=1080)return false;
        if(!validVmxPayload(bits,payload,header.height,preview))return false;
        f.Width=header.width;f.Height=header.height;f.Flags=static_cast<OMTVideoFlags>(header.flags);
        f.FrameRateN=header.frameRateN;f.FrameRateD=header.frameRateD;f.AspectRatio=header.aspectRatio;
        f.ColorSpace=static_cast<OMTColorSpace>(header.colorSpace);f.Codec=OMTCodec_VMX1;
        if(currentFlags&(OMTReceiveFlags_IncludeCompressed|OMTReceiveFlags_CompressedOnly)) {
            out.compressed.assign(bits,bits+payload);f.CompressedData=out.compressed.data();f.CompressedLength=static_cast<int>(payload);
        }
        if(currentFlags&OMTReceiveFlags_CompressedOnly)return true;
        if(!decoder || decoderW!=header.width || decoderH!=header.height || decoderColor!=header.colorSpace) {
            if(decoder)VMX_Destroy(decoder);decoder=nullptr;
            decoder=VMX_Create({header.width,header.height},VMX_PROFILE_DEFAULT,static_cast<VMX_COLORSPACE>(header.colorSpace));
            if(!decoder)return false;
            decoderW=header.width;decoderH=header.height;decoderColor=header.colorSpace;
        }
        if(VMX_LoadFrom(decoder,bits,static_cast<int>(payload))!=VMX_ERR_OK)return false;
        if(preview){f.Width=(header.width/8)&~1;f.Height=header.height/8;if(interlaced)f.Height&=~1;}
        if(f.Width<=0 || f.Height<=0)return false;
        const bool bgra=format==OMTPreferredVideoFormat_BGRA || (format==OMTPreferredVideoFormat_UYVYorBGRA && alpha);
        const bool p216=!preview && (format==OMTPreferredVideoFormat_P216 ||
            (format==OMTPreferredVideoFormat_UYVYorUYVAorP216orPA16 && high));
        const bool planarAlpha=alpha && format!=OMTPreferredVideoFormat_P216 &&
            (format==OMTPreferredVideoFormat_UYVYorUYVA || format==OMTPreferredVideoFormat_UYVYorUYVAorP216orPA16);
        f.Codec=bgra?OMTCodec_BGRA:p216?(planarAlpha?OMTCodec_PA16:OMTCodec_P216):
                                                 (planarAlpha?OMTCodec_UYVA:OMTCodec_UYVY);
        f.Stride=f.Width*(bgra?4:2);
        size_t bytes=static_cast<size_t>(f.Width)*f.Height*(bgra?4:p216?(planarAlpha?6:4):(planarAlpha?3:2));
        out.pixels.assign(bytes,0);
        VMX_ERR result=VMX_ERR_UNKNOWN;
        if(preview) {
            if(bgra)result=alpha?VMX_DecodePreviewBGRA(decoder,out.pixels.data(),f.Stride):VMX_DecodePreviewBGRX(decoder,out.pixels.data(),f.Stride);
            else result=planarAlpha?VMX_DecodePreviewUYVA(decoder,out.pixels.data(),f.Stride):VMX_DecodePreviewUYVY(decoder,out.pixels.data(),f.Stride);
        } else if(bgra)result=alpha?VMX_DecodeBGRA(decoder,out.pixels.data(),f.Stride):VMX_DecodeBGRX(decoder,out.pixels.data(),f.Stride);
        else if(p216)result=planarAlpha?VMX_DecodePA16(decoder,out.pixels.data(),f.Stride):VMX_DecodeP216(decoder,out.pixels.data(),f.Stride);
        else result=planarAlpha?VMX_DecodeUYVA(decoder,out.pixels.data(),f.Stride):VMX_DecodeUYVY(decoder,out.pixels.data(),f.Stride);
        if(result!=VMX_ERR_OK)return false;
    }
    f.Data=out.pixels.data();f.DataLength=static_cast<int>(out.pixels.size());return true;
}

Receiver::Receiver(const char* a,OMTFrameType t,OMTPreferredVideoFormat f,OMTReceiveFlags flags):
    impl_(new Impl(a,static_cast<int>(t),f,static_cast<int>(flags))) {}
Receiver::~Receiver()=default;
bool Receiver::start() {
    auto& s=*impl_;
    if(s.running || s.types<=0 || (s.types&~7) || s.flags<0 || (s.flags&~7) ||
       s.format<0 || s.format>OMTPreferredVideoFormat_P216 || !s.resolve())return false;
    s.running=true;
    try {
        if(s.types&3)s.workers[0]=std::thread([&s]{s.run(0);});
        if(s.types&4)s.workers[1]=std::thread([&s]{s.run(1);});
    } catch(...) {s.running=false;s.cv.notify_all();throw;}
    return true;
}
OMTMediaFrame* Receiver::receive(OMTFrameType types,int timeoutMs) {
    auto& s=*impl_;int mask=static_cast<int>(types)&s.types;
    if(!mask || timeoutMs<0)return nullptr;
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    for(;;) {
        Impl::Packet packet;int chosen=-1,flags=0;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            auto select=[&]{
                uint64_t order=UINT64_MAX;chosen=-1;
                for(int i=0;i<3;++i)if((mask&typeOf(i)) && !s.queues[i].empty() && s.queues[i].front().order<order) {
                    chosen=i;order=s.queues[i].front().order;
                }
                return chosen>=0 || !s.running;
            };
            if(!select() && !s.cv.wait_until(lock,deadline,select))return nullptr;
            if(chosen<0)return nullptr;
            packet=std::move(s.queues[chosen].front());s.queues[chosen].pop_front();flags=s.flags;
        }
        auto& output=s.outputs[chosen];std::lock_guard<std::mutex> outputLock(output.mutex);
        const auto begin=Clock::now();bool ok=s.decode(packet,chosen,output,flags);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.stats[chosen].CodecTime+=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count();
            if(!ok)s.stats[chosen].FramesDropped++;
        }
        if(ok)return &output.frame;
        if(Clock::now()>=deadline)return nullptr;
    }
}
int Receiver::sendMetadata(const OMTMediaFrame& frame) {
    if(frame.Type!=OMTFrameType_Metadata || !validMetadata(frame.Data,frame.DataLength))return 0;
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.running || (!s.connected[0] && !s.connected[1]) || s.outgoing.size()>=60)return 0;
    FrameHeader h{1,1,frame.Timestamp,0,frame.DataLength-1};
    std::vector<uint8_t> bytes(sizeof(h)+h.dataLength);
    std::memcpy(bytes.data(),&h,sizeof(h));std::memcpy(bytes.data()+sizeof(h),frame.Data,h.dataLength);
    s.outgoing.push_back(std::move(bytes));return frame.DataLength;
}
void Receiver::setTally(const OMTTally& tally) {
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    s.localTally={tally.preview?1:0,tally.program?1:0};++s.settingsVersion;
}
int Receiver::getTally(int timeoutMs,OMTTally& tally) {
    auto& s=*impl_;std::unique_lock<std::mutex> lock(s.mutex);
    s.cv.wait_for(lock,std::chrono::milliseconds((std::max)(0,timeoutMs)),[&]{return s.tallyChanged || !s.running;});
    tally=s.remoteTally;int changed=timeoutMs>0 && s.tallyChanged?1:0;
    if(timeoutMs>0)s.tallyChanged=false;return changed;
}
void Receiver::setFlags(OMTReceiveFlags flags) {
    int value=static_cast<int>(flags);if(value<0 || (value&~7))return;
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);s.flags=value;++s.settingsVersion;
}
void Receiver::setQuality(OMTQuality quality) {
    if(quality!=OMTQuality_Default && quality!=OMTQuality_Low && quality!=OMTQuality_Medium && quality!=OMTQuality_High)return;
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);s.quality=quality;++s.settingsVersion;
}
void Receiver::senderInfo(OMTSenderInfo& info) {
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);info=s.info;
}
void Receiver::statistics(bool audio,OMTStatistics& stats) {
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    int i=audio?1:0;stats=s.stats[i];
    stats.BytesReceivedSinceLast=stats.BytesReceived-s.previous[i].BytesReceived;
    stats.FramesSinceLast=stats.Frames-s.previous[i].Frames;
    stats.CodecTimeSinceLast=stats.CodecTime-s.previous[i].CodecTime;s.previous[i]=s.stats[i];
}
}
