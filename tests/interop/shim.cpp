#include "OMTSender.h"
extern "C" __declspec(dllexport) omt_send_t* test_create() {
    omt::SenderConfig config;config.bindAddress="127.0.0.1";config.port=26490;
    auto* sender=new omt::Sender(config);
    if(!sender->start()){delete sender;return nullptr;}
    return reinterpret_cast<omt_send_t*>(sender);
}
