#include "wire_peer.h"
#include "vmxcodec.h"
#include <cstdio>

static void require(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) throw std::runtime_error(text);
}
int main() try {
    omt::SenderConfig config;config.bindAddress="127.0.0.1";config.portStart=26400;config.portEnd=26500;
    omt::Sender sender(config);
    require(sender.start(), "sender starts");
    WirePeer full(sender.port()), preview(sender.port());
    preview.command(omt::MetadataConstants::CHANNEL_PREVIEW_VIDEO_ON);
    full.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO);
    preview.command(omt::MetadataConstants::CHANNEL_SUBSCRIBE_VIDEO);
    require(waitUntil([&] { return sender.videoSubscriberCount() == 2; }), "subscriptions processed");
    constexpr int w = 320, h = 176;
    std::vector<uint8_t> raster(w*h*4);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        size_t i = (y*w+x)*4;
        raster[i]=x%256; raster[i+1]=y; raster[i+2]=(x+y)%256; raster[i+3]=128;
    }
    omt::MediaFrame frame{};
    frame.data=raster.data(); frame.dataLength=static_cast<int>(raster.size());
    frame.width=w; frame.height=h; frame.stride=w*4; frame.codec=omt::Codec::BGRA;
    frame.flags=omt::VideoFlags::Alpha; frame.colorSpace=709;
    frame.frameRateN=60000; frame.frameRateD=1001; frame.timestamp=123456;
    require(sender.send(frame)>0, "real VMX video encoded");
    omt::FrameHeader fh{},ph{};
    auto fb=full.frame(2,fh), pb=preview.frame(2,ph);
    omt::VideoExtHeader fv{},pv{};
    require(fb.size()>sizeof(fv)&&pb.size()>sizeof(pv), "video headers available");
    std::memcpy(&fv,fb.data(),sizeof(fv)); std::memcpy(&pv,pb.data(),sizeof(pv));
    require(pb.size()<fb.size(), "preview uses fewer wire bytes");
    require((pv.flags & omt::VideoFlags::Preview) && !(fv.flags & omt::VideoFlags::Preview),
            "preview and full subscribers have distinct flags");
    require(pv.width==w && pv.height==h && ph.timestamp==123456 && pv.frameRateN==60000 &&
            pv.frameRateD==1001 && pv.colorSpace==709, "preview retains original geometry and timing header");
    auto destroy=[](VMX_INSTANCE* p){VMX_Destroy(p);};
    std::unique_ptr<VMX_INSTANCE,decltype(destroy)> decoder(
        VMX_Create({w,h},VMX_PROFILE_OMT_SQ,VMX_COLORSPACE_BT709),destroy);
    require(decoder!=nullptr,"independent VMX decoder created");
    std::vector<uint8_t> expected(w/8*h/8*4), actual(expected.size());
    require(VMX_LoadFrom(decoder.get(),fb.data()+32,static_cast<int>(fb.size()-32))==VMX_ERR_OK &&
            VMX_DecodePreviewBGRA(decoder.get(),expected.data(),w/8*4)==VMX_ERR_OK,
            "full stream provides reference preview pixels");
    require(VMX_LoadFrom(decoder.get(),pb.data()+32,static_cast<int>(pb.size()-32))==VMX_ERR_OK &&
            VMX_DecodePreviewBGRA(decoder.get(),actual.data(),w/8*4)==VMX_ERR_OK && expected==actual,
            "truncated preview independently decodes to identical pixels including alpha");
    preview.command(omt::MetadataConstants::CHANNEL_PREVIEW_VIDEO_OFF);
    preview.command(omt::MetadataConstants::TALLY_PROGRAM_LEGACY);
    require(waitUntil([&]{return sender.hasTallyProgram();}),"preview toggle processed before tally barrier");
    require(sender.send(frame)>0,"second frame encoded");
    fb=full.frame(2,fh); pb=preview.frame(2,ph);
    require(fb==pb,"disabling preview restores full identical media on existing connection");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"FAIL %s\n",e.what()); return 1;
}
