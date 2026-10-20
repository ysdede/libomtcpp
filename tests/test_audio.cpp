#include "wire_peer.h"
#include "../src/OMTAudio.h"
#include "libomt.h"
#include <cstdio>

static void require(bool ok,const char* text) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",text);
    if(!ok)throw std::runtime_error(text);
}
int main() try {
    auto destroy=[](omt_send_t* p){omt_send_destroy(p);};
    std::unique_ptr<omt_send_t,decltype(destroy)> sender(omt_send_create("audio-test",OMTQuality_Default),destroy);
    require(sender!=nullptr,"C sender starts");
    WirePeer audio(omt_send_getport(sender.get())), video(omt_send_getport(sender.get()));
    audio.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_AUDIO);
    audio.command(omt::MetadataConstants::TALLY_PROGRAM_LEGACY);
    video.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO);
    video.command(omt::MetadataConstants::TALLY_PREVIEW_LEGACY);
    require(waitUntil([&] { OMTTally t{};omt_send_gettally(sender.get(),0,&t);return t.preview&&t.program; }),
            "both subscription commands processed");
    // Four planes: +1/-1, silence, -0/NaN and silence. Preserve bits exactly.
    std::vector<uint32_t> input{0x3f800000,0xbf800000,0,0,0x80000000,0x7fc01234,0,0};
    OMTMediaFrame f{};f.Type=OMTFrameType_Audio;f.Codec=OMTCodec_FPA1;f.SampleRate=48000;
    f.Channels=4;f.SamplesPerChannel=2;f.Timestamp=123456;
    f.Data=input.data();f.DataLength=static_cast<int>(input.size()*4);
    const char meta[]="<Frame id=\"7\" />";
    f.FrameMetadata=const_cast<char*>(meta);f.FrameMetadataLength=sizeof(meta);
    require(omt_send(sender.get(),&f)>0,"C API sends FPA1 audio");
    omt::FrameHeader h{};auto bytes=audio.frame(4,h);
    require(h.timestamp==123456&&h.metadataLength==sizeof(meta)&&bytes.size()==24+16+sizeof(meta),
            "audio type, timestamp, 24-byte header and metadata lengths");
    const uint8_t expectedPrefix[]{'F','P','A','1',0x80,0xbb,0,0,2,0,0,0,4,0,0,0,5,0,0,0,0,0,0,0};
    require(std::memcmp(bytes.data(),expectedPrefix,24)==0,"independent golden FPA1 header and active mask");
    require(std::memcmp(bytes.data()+24,input.data(),8)==0 &&
            std::memcmp(bytes.data()+32,input.data()+4,8)==0 &&
            std::memcmp(bytes.data()+40,meta,sizeof(meta))==0,"only active planes followed by unchanged metadata");
    omt::AudioExtHeader ext{};std::memcpy(&ext,bytes.data(),24);
    std::vector<uint8_t> decoded;
    require(omt::decodeAudio(ext,bytes.data()+24,16,decoded)&&decoded.size()==input.size()*4 &&
            std::memcmp(decoded.data(),input.data(),decoded.size())==0,"planar decode restores silent planes and all sample bits");
    OMTStatistics a{},v{};
    require(waitUntil([&]{omt_send_getaudiostatistics(sender.get(),&a);return a.Frames==1;}),"audio completion statistics");
    omt_send_getvideostatistics(sender.get(),&v);
    require(v.Frames==0&&v.BytesSent==0&&a.BytesSent==static_cast<int64_t>(16+bytes.size()),
            "audio does not enter video statistics or route to video-only client");
    std::vector<uint32_t> all(64,0);all[62]=0x3f800000;f.Channels=32;f.Data=all.data();f.DataLength=256;
    f.FrameMetadata=nullptr;f.FrameMetadataLength=0;
    require(omt_send(sender.get(),&f)>0,"channel 32 encoded");
    bytes=audio.frame(4,h);std::memcpy(&ext,bytes.data(),24);
    require(ext.activeChannels==0x80000000u&&bytes.size()==32,"channel 32 uses bit 31, with no signed-shift overflow");
    all[62]=0;require(omt_send(sender.get(),&f)>0,"all-silent frame still sent");
    bytes=audio.frame(4,h);std::memcpy(&ext,bytes.data(),24);
    require(bytes.size()==24&&ext.activeChannels==0&&omt::decodeAudio(ext,nullptr,0,decoded)&&
            decoded.size()==256&&std::memcmp(decoded.data(),all.data(),256)==0,"zero-payload silence retains timing and geometry");
    f.Channels=33;require(omt_send(sender.get(),&f)==0,"reject channel count above 32");
    f.Channels=2;f.SamplesPerChannel=0x7fffffff;require(omt_send(sender.get(),&f)==0,"reject oversized sample count before access");
    f.SamplesPerChannel=2;f.DataLength=1;require(omt_send(sender.get(),&f)==0,"reject truncated input");
    ext.channels=1;ext.activeChannels=2;require(!omt::decodeAudio(ext,nullptr,0,decoded),"reject active bits outside channel count");
    ext.activeChannels=1;require(!omt::decodeAudio(ext,nullptr,0,decoded),"reject missing active plane");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
