#include "wire_peer.h"
#include <cstdio>
static int failures=0;
static void check(bool ok,const char* label) { std::printf("%s %s\n",ok?"PASS":"FAIL",label);if(!ok)++failures; }
int main() try {
    omt::SenderConfig cfg;cfg.bindAddress="127.0.0.1";
    omt::Sender sender(cfg);
    if(!sender.start())throw std::runtime_error("start");
    auto validate=[&] {
        float sample=1;omt::AudioFrame f{};f.data=&sample;f.dataLength=4;f.channels=1;f.samplesPerChannel=1;
        check(sender.sendAudio(f)==0,"valid audio without subscribers returns zero bytes");
        f.data=nullptr;check(sender.sendAudio(f)==0,"null audio rejected without subscribers");f.data=&sample;
        f.dataLength=3;check(sender.sendAudio(f)==0,"truncated audio rejected without subscribers");f.dataLength=4;
        f.channels=33;check(sender.sendAudio(f)==0,"invalid channels rejected without subscribers");f.channels=1;
        f.samplesPerChannel=0x7fffffff;check(sender.sendAudio(f)==0,"oversized audio rejected without subscribers");
    };
    validate();
    WirePeer peer(sender.port());peer.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO);
    peer.command(omt::MetadataConstants::TALLY_PROGRAM_LEGACY);
    if(!waitUntil([&]{return sender.hasTallyProgram();}))throw std::runtime_error("command fence");
    validate();
    peer.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_AUDIO);
    peer.command(omt::MetadataConstants::TALLY_PREVIEW_LEGACY);
    if(!waitUntil([&]{return sender.hasTallyPreview();}))throw std::runtime_error("audio command fence");
    float sample=1;omt::AudioFrame f{};f.data=&sample;f.dataLength=4;f.channels=1;f.samplesPerChannel=1;f.timestamp=42;
    check(sender.sendAudio(f)>0,"audio begins on existing connection after subscription");
    omt::FrameHeader h{};auto bytes=peer.frame(4,h);
    check(h.timestamp==42 && bytes.size()==28 && std::memcmp(bytes.data()+24,&sample,4)==0,"subscribed audio wire payload");
    return failures?1:0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
