#include "wire_peer.h"
#include <cstdio>
static void require(bool ok,const char* label) {std::printf("%s %s\n",ok?"PASS":"FAIL",label);if(!ok)throw std::runtime_error(label);}
int main() try {
    omt::SenderConfig cfg;cfg.bindAddress="127.0.0.1";
    omt::Sender sender(cfg);require(sender.start(),"sender starts");
    std::vector<uint8_t> data(320*176*4,128);
    omt::MediaFrame f{};f.data=data.data();f.dataLength=int(data.size());f.width=320;f.height=176;
    f.stride=320*4;f.codec=omt::Codec::BGRA;f.frameRateN=30;f.frameRateD=1;f.colorSpace=709;
    require(sender.send(f)==1,"no connection skips encoding");
    WirePeer peer(sender.port());peer.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_METADATA);
    peer.command(omt::MetadataConstants::TALLY_PROGRAM);
    require(waitUntil([&]{return sender.hasTallyProgram();}),"metadata subscription fence");
    // This target links the deliberately failing VMX stub. Reaching the codec
    // returns zero, so successful skips prove the encoder was bypassed.
    require(sender.send(f)==1,"metadata-only subscriber skips failing codec");
    peer.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_AUDIO);
    peer.command(omt::MetadataConstants::TALLY_PREVIEW);
    require(waitUntil([&]{return sender.hasTallyPreview();}),"audio subscription fence");
    require(sender.send(f)==1,"audio-only subscriber skips failing codec");
    f.dataLength=1;require(sender.send(f)==0,"invalid video still rejected with connected peers");f.dataLength=int(data.size());
    peer.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO);
    require(waitUntil([&]{return sender.videoSubscriberCount()==1;}),"video subscription fence");
    require(sender.send(f)==0,"late video subscriber reaches deliberately failing codec");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
