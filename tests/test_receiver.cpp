#include "libomt.h"
#include "vmxcodec.h"
#include "wire_peer.h"
#include <cstdio>
#include <memory>
#ifndef _WIN32
#include <poll.h>
#endif

static int checks=0;
static void require(bool ok,const char* text) {
    ++checks;
    if(!ok)throw std::runtime_error(text);
}
using ReceivePtr=std::unique_ptr<omt_receive_t,decltype(&omt_receive_destroy)>;
static ReceivePtr receiver(int port,int types,int format=2,int flags=0) {
    const auto address=std::string("omt://127.0.0.1:")+std::to_string(port);
    ReceivePtr result(omt_receive_create(address.c_str(),static_cast<OMTFrameType>(types),
        static_cast<OMTPreferredVideoFormat>(format),static_cast<OMTReceiveFlags>(flags)),omt_receive_destroy);
    require(result!=nullptr,"receiver creation");return result;
}
struct Listener {
    int fd=-1,port=0;
    Listener() {
        fd=static_cast<int>(socket(AF_INET,SOCK_STREAM,0));
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0 && listen(fd,4)==0,"listener");
        socklen_t n=sizeof(a);getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n);port=ntohs(a.sin_port);
    }
    ~Listener(){close(fd);}
    int acceptPeer() {
        pollfd p{};p.fd=fd;p.events=POLLIN;require(poll(&p,1,3000)>0,"receiver connects/reconnects");
        int peer=static_cast<int>(accept(fd,nullptr,nullptr));require(peer>=0,"accept");
#ifdef _WIN32
        DWORD timeout=2000;
#else
        timeval timeout{2,0};
#endif
        setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        return peer;
    }
};
struct Peer {
    int fd;
    explicit Peer(int socket):fd(socket){}
    ~Peer(){if(fd>=0)close(fd);}
    void closeNow(){shutdown(fd,SHUT_RDWR);close(fd);fd=-1;}
    void sendBytes(const std::vector<uint8_t>& bytes,size_t fragment=SIZE_MAX) {
        size_t n=0;
        while(n<bytes.size()) {
            int sent=send(fd,bytes.data()+n,static_cast<int>((std::min)(fragment,bytes.size()-n)),MSG_NOSIGNAL);
            require(sent>0,"fixture send");n+=sent;
        }
    }
    void read(void* data,size_t bytes) {
        auto* p=static_cast<char*>(data);
        while(bytes){int n=recv(fd,p,static_cast<int>(bytes),0);require(n>0,"control receive");p+=n;bytes-=n;}
    }
    std::string metadata(int64_t* timestamp=nullptr) {
        omt::FrameHeader h{};read(&h,16);
        require(h.version==1 && h.frameType==1 && h.dataLength>0 && h.dataLength<=65535 &&
                h.metadataLength==0,"official control framing");
        std::string xml(h.dataLength,'\0');read(xml.data(),xml.size());
        require(xml.find('\0')==std::string::npos,"official control XML has no wire NUL");
        if(timestamp)*timestamp=h.timestamp;return xml;
    }
    void initialized(bool audio) {
        bool subscribe=false;
        for(int i=0;i<8;++i) {
            const auto xml=metadata();
            if(xml==(audio?omt::MetadataConstants::CHANNEL_SUBSCRIBE_AUDIO:omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO))subscribe=true;
            if(xml==omt::MetadataConstants::TALLY_NONE_LEGACY){require(subscribe,"subscription before tally");return;}
        }
        require(false,"initial control commands");
    }
};
// Independent little-endian wire fixture. It does not serialize production headers.
static void u32(std::vector<uint8_t>& bytes,uint32_t v){for(int i=0;i<4;++i)bytes.push_back(static_cast<uint8_t>(v>>(8*i)));}
static std::vector<uint8_t> packet(int type,int64_t time,const std::vector<uint8_t>& payload,int metadata=0) {
    std::vector<uint8_t> out{1,static_cast<uint8_t>(type)};
    for(int i=0;i<8;++i)out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(time)>>(8*i)));
    out.push_back(metadata&255);out.push_back(metadata>>8);u32(out,static_cast<uint32_t>(payload.size()));
    out.insert(out.end(),payload.begin(),payload.end());return out;
}
static std::vector<uint8_t> audio(int64_t time,bool invalid=false) {
    std::vector<uint8_t> payload;
    u32(payload,0x31415046);u32(payload,48000);u32(payload,2);u32(payload,2);
    u32(payload,invalid?4:1);u32(payload,0);u32(payload,0x3f800000);u32(payload,0xbf800000);
    payload.insert(payload.end(),{'<','a','/','>',0});return packet(4,time,payload,5);
}
static void xml(Peer& peer,const std::string& value,bool terminated=false) {
    std::vector<uint8_t> payload(value.begin(),value.end());if(terminated)payload.push_back(0);
    peer.sendBytes(packet(1,777,payload),3);
}

static void wireTests() {
    require(!omt_receive_create("omt://127.0.0.1:65536",OMTFrameType_Video,OMTPreferredVideoFormat_BGRA,OMTReceiveFlags_None),"invalid port");
    Listener listener;
    auto r=receiver(listener.port,4);
    Peer peer(listener.acceptPeer());peer.initialized(true);
    xml(peer,omt::MetadataConstants::TALLY_NONE_LEGACY);
    OMTTally tally{};
    require(omt_receive_gettally(r.get(),2000,&tally)==1 && tally.preview==0 && tally.program==0,"initial all-off tally event");
    require(omt_receive_gettally(r.get(),5,&tally)==0,"no duplicate tally event");
    xml(peer,omt::MetadataConstants::TALLY_PREVIEWPROGRAM_LEGACY,true);
    require(omt_receive_gettally(r.get(),2000,&tally)==1 && tally.preview && tally.program,"NUL-terminated legacy tally accepted");
    xml(peer,"<OMTInfo ProductName=\"Fixture &amp; Sender\" Manufacturer=\"Official wire\" Version=\"1\" />");
    require(waitUntil([&]{OMTSenderInfo i{};omt_receive_getsenderinformation(r.get(),&i);return std::string(i.ProductName)=="Fixture & Sender";}),"sender info XML");
    peer.sendBytes(audio(123),1);
    auto* frame=omt_receive(r.get(),OMTFrameType_Audio,2000);
    require(frame && frame->Timestamp==123 && frame->DataLength==16 && frame->SampleRate==48000 &&
            frame->Channels==2 && frame->SamplesPerChannel==2,"fragmented FPA1 frame");
    const uint32_t expected[]{0x3f800000,0xbf800000,0,0};
    require(std::memcmp(frame->Data,expected,16)==0,"independent FPA1 golden samples");
    require(frame->FrameMetadataLength==5 && std::memcmp(frame->FrameMetadata,"<a/>",5)==0,"per-frame metadata");
    peer.sendBytes(audio(124,true));
    require(!omt_receive(r.get(),OMTFrameType_Audio,100),"invalid active mask rejected");
    for(int i=0;i<15;++i)peer.sendBytes(audio(200+i));
    require(waitUntil([&]{OMTStatistics s{};omt_receive_getaudiostatistics(r.get(),&s);return s.Frames>=17;}),"queued fixture received");
    OMTStatistics stats{};omt_receive_getaudiostatistics(r.get(),&stats);
    require(stats.FramesDropped==6,"decode rejection plus bounded queue overflow");
    for(int i=0;i<10;++i) {
        frame=omt_receive(r.get(),OMTFrameType_Audio,100);
        require(frame && frame->Timestamp==205+i,"queue retains ten newest frames in order");
    }
    char request[]="<application value=\"1\" />";
    OMTMediaFrame outbound{};outbound.Type=OMTFrameType_Metadata;outbound.Data=request;
    outbound.DataLength=sizeof(request);outbound.Timestamp=999;
    require(omt_receive_send(r.get(),&outbound)>0,"receiver metadata send accepted");
    int64_t timestamp=0;require(peer.metadata(&timestamp)==request && timestamp==999,"receiver metadata timestamp and exact XML");
    // An oversized advertised length must close promptly without allocating its body.
    auto bad=packet(4,0,{});bad[12]=0xff;bad[13]=0xff;bad[14]=0xff;bad[15]=0x7f;
    peer.sendBytes(bad);peer.closeNow();
    Peer next(listener.acceptPeer());next.initialized(true);
    next.sendBytes(audio(300));
    frame=omt_receive(r.get(),OMTFrameType_Audio,2000);
    require(frame && frame->Timestamp==300,"receiver reconnects after malformed header");
    auto begin=std::chrono::steady_clock::now();r.reset();
    require(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"idle socket shutdown bounded");
    r=receiver(listener.port,2); // stop while connecting/initializing, never detach workers
    begin=std::chrono::steady_clock::now();r.reset();
    require(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"connect shutdown bounded");
}

static void mediaTests() {
    omt::SenderConfig config;config.bindAddress="127.0.0.1";config.portStart=26400;config.portEnd=26500;
    omt::Sender sender(config);require(sender.start(),"media sender");
    constexpr int w=320,h=176;
    std::vector<uint8_t> raster(w*h*4);
    for(size_t i=0;i<raster.size();i+=4){raster[i]=i%239;raster[i+1]=i%127;raster[i+2]=75;raster[i+3]=128;}
    omt::MediaFrame source{};
    source.data=raster.data();source.dataLength=static_cast<int>(raster.size());source.width=w;source.height=h;
    source.stride=w*4;source.codec=omt::Codec::BGRA;source.frameRateN=60000;source.frameRateD=1001;source.colorSpace=709;
    for(int alpha=0;alpha<2;++alpha)for(int preview=0;preview<2;++preview)for(int format=0;format<6;++format) {
        source.flags=alpha?omt::VideoFlags::Alpha:0;
        source.codec=alpha?omt::Codec::BGRA:omt::Codec::BGRX;
        auto r=receiver(sender.port(),2,format,2|preview);
        require(waitUntil([&]{return sender.videoSubscriberCount()==1;}),"video subscribed");
        source.timestamp=10000+alpha*100+preview*10+format;
        OMTMediaFrame* frame=nullptr;
        require(waitUntil([&]{sender.send(source);frame=omt_receive(r.get(),OMTFrameType_Video,20);return frame!=nullptr;}),"video decoded");
        require(frame->Width==(preview?w/8:w) && frame->Height==(preview?h/8:h) &&
                frame->Timestamp==source.timestamp && frame->FrameRateN==60000 && frame->FrameRateD==1001 &&
                frame->CompressedLength>0 && frame->CompressedData,"receiver geometry timing compressed retention");
        const int expectedCodec=(format==2 || (format==1 && alpha))?OMTCodec_BGRA:
            (!preview && format==5)?OMTCodec_P216:
            (alpha && (format==3 || format==4))?OMTCodec_UYVA:OMTCodec_UYVY;
        require(frame->Codec==expectedCodec,"preferred format contract");
        auto* decoder=VMX_Create({w,h},VMX_PROFILE_DEFAULT,VMX_COLORSPACE_BT709);
        require(decoder && VMX_LoadFrom(decoder,static_cast<uint8_t*>(frame->CompressedData),frame->CompressedLength)==VMX_ERR_OK,"reference decoder");
        std::vector<uint8_t> expected(frame->DataLength,0);
        VMX_ERR result=VMX_ERR_UNKNOWN;
        if(frame->Codec==OMTCodec_BGRA)
            result=preview?(alpha?VMX_DecodePreviewBGRA(decoder,expected.data(),frame->Stride):VMX_DecodePreviewBGRX(decoder,expected.data(),frame->Stride)):
                           (alpha?VMX_DecodeBGRA(decoder,expected.data(),frame->Stride):VMX_DecodeBGRX(decoder,expected.data(),frame->Stride));
        else if(frame->Codec==OMTCodec_UYVA)
            result=preview?VMX_DecodePreviewUYVA(decoder,expected.data(),frame->Stride):VMX_DecodeUYVA(decoder,expected.data(),frame->Stride);
        else if(frame->Codec==OMTCodec_P216)result=VMX_DecodeP216(decoder,expected.data(),frame->Stride);
        else if(frame->Codec==OMTCodec_UYVY)
            result=preview?VMX_DecodePreviewUYVY(decoder,expected.data(),frame->Stride):VMX_DecodeUYVY(decoder,expected.data(),frame->Stride);
        require(result==VMX_ERR_OK && std::memcmp(frame->Data,expected.data(),expected.size())==0,"decoded pixels equal independent VMX oracle");
        VMX_Destroy(decoder);
        r.reset();require(waitUntil([&]{sender.send(source);return sender.videoSubscriberCount()==0;}),"receiver closes old subscription");
    }
    auto r=receiver(sender.port(),6,2,4);
    require(waitUntil([&]{return sender.connectionCount()>=2 && sender.videoSubscriberCount()==1;}),"audio/video independent connections");
    OMTTally tally{1,1};omt_receive_settally(r.get(),&tally);
    require(waitUntil([&]{return sender.hasTallyProgram();}),"receiver tally reaches sender");
    char request[]="<user message=\"hello\" />";
    OMTMediaFrame meta{};meta.Type=OMTFrameType_Metadata;meta.Timestamp=456;meta.Data=request;meta.DataLength=sizeof(request);
    require(omt_receive_send(r.get(),&meta)>0,"application metadata send");
    auto* received=sender.receiveMetadata(2000);
    require(received && received->Timestamp==456 && std::strcmp(static_cast<char*>(received->Data),request)==0,"sender receives application metadata");
    require(sender.send(source)>0,"compressed-only frame sent");
    auto* video=omt_receive(r.get(),OMTFrameType_Video,2000);
    require(video && !video->Data && video->DataLength==0 && video->CompressedData && video->Codec==OMTCodec_VMX1,"compressed-only ownership");
    std::vector<uint8_t> retained(static_cast<uint8_t*>(video->CompressedData),static_cast<uint8_t*>(video->CompressedData)+video->CompressedLength);
    const float samples[]{1,-1,0,0};
    omt::AudioFrame audioFrame;audioFrame.data=samples;audioFrame.dataLength=sizeof(samples);audioFrame.channels=2;audioFrame.samplesPerChannel=2;
    require(sender.sendAudio(audioFrame)>0,"audio sent");
    auto* sound=omt_receive(r.get(),OMTFrameType_Audio,2000);
    require(sound && sound->DataLength==sizeof(samples) && std::memcmp(sound->Data,samples,sizeof(samples))==0,"audio decoded alongside video");
    require(std::memcmp(video->CompressedData,retained.data(),retained.size())==0,"audio receive preserves video pointers");
    tally={0,0};omt_receive_settally(r.get(),&tally);
    require(waitUntil([&]{return !sender.hasTallyProgram() && !sender.hasTallyPreview();}),"cleared tally reaches sender");
    require(sender.getTally(1,tally)==1 && !tally.program && !tally.preview,"sender all-off tally change notification");
}
static void edgeMediaTests() {
    for(int high=0;high<2;++high) {
        const int w=320,h=high?32:1080;
        auto destroy=[](VMX_INSTANCE* p){VMX_Destroy(p);};
        std::unique_ptr<VMX_INSTANCE,decltype(destroy)> codec(
            VMX_Create({w,h},VMX_PROFILE_OMT_SQ,VMX_COLORSPACE_BT709),destroy);
        require(codec!=nullptr,"edge codec created");
        if(high) {
            std::vector<uint16_t> planar(w*h*3,32768);
            std::fill(planar.begin()+w*h*2,planar.end(),49152);
            require(VMX_EncodePA16(codec.get(),reinterpret_cast<BYTE*>(planar.data()),w*2,0)==VMX_ERR_OK,"real high-bit alpha encode");
        } else {
            std::vector<BYTE> bgra(w*h*4,100);
            require(VMX_EncodeBGRA(codec.get(),bgra.data(),w*4,1)==VMX_ERR_OK,"real interlaced encode");
        }
        std::vector<uint8_t> compressed(w*h*4);
        int length=VMX_SaveTo(codec.get(),compressed.data(),static_cast<int>(compressed.size()));
        int prefix=VMX_GetEncodedPreviewLength(codec.get());
        require(length>prefix && prefix>0,"full and DC prefix lengths");
        for(int preview=0;preview<(high?1:2);++preview) {
            Listener listener;auto r=receiver(listener.port,2,high?4:2,preview);
            Peer peer(listener.acceptPeer());peer.initialized(false);
            auto make=[&](bool usePrefix,bool flagPreview) {
                std::vector<uint8_t> bytes;
                u32(bytes,0x31584d56);u32(bytes,w);u32(bytes,h);u32(bytes,50);u32(bytes,1);
                u32(bytes,0);u32(bytes,2|(high?16:1)|(flagPreview?8:0));u32(bytes,709);
                bytes.insert(bytes.end(),compressed.begin(),compressed.begin()+(usePrefix?prefix:length));
                return packet(2,555,bytes);
            };
            peer.sendBytes(make(preview!=0,preview!=0),1024);
            auto* f=omt_receive(r.get(),OMTFrameType_Video,2000);
            const int outW=preview?40:w,outH=preview?134:h;
            require(f && f->Width==outW && f->Height==outH &&
                    f->Codec==(high?OMTCodec_PA16:OMTCodec_BGRA),"interlace/high-bit receiver output contract");
            std::vector<uint8_t> expected(outW*outH*(high?6:4));
            require(VMX_LoadFrom(codec.get(),compressed.data(),preview?prefix:length)==VMX_ERR_OK,"edge reference loads transmitted coefficients");
            auto result=high?VMX_DecodePA16(codec.get(),expected.data(),w*2):
                preview?VMX_DecodePreviewBGRA(codec.get(),expected.data(),outW*4):
                        VMX_DecodeBGRA(codec.get(),expected.data(),w*4);
            require(result==VMX_ERR_OK && f->DataLength==static_cast<int>(expected.size()) &&
                    std::memcmp(f->Data,expected.data(),expected.size())==0,"interlaced preview/high-bit alpha pixel oracle");
            peer.sendBytes(make(true,false));
            require(!omt_receive(r.get(),OMTFrameType_Video,100),"DC-only payload cannot reuse stale AC as a full frame");
        }
    }
}
int main() try {
    wireTests();mediaTests();edgeMediaTests();std::printf("PASS receiver: %d checks\n",checks);return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL receiver: %s (%d checks)\n",e.what(),checks);return 1;}
