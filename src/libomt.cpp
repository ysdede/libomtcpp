/**
 * libomt.cpp - C API Wrapper for libomtcpp
 * 
 * This file provides the C API defined in libomt.h
 * by wrapping the C++ omt::Sender and omt::Discovery classes.
 * 
 * Cross-platform compatible: Android, Linux, macOS, Windows
 */

#include "libomt.h"
#include "OMTSender.h"
#include "OMTReceiver.h"
#include <memory>
#include "OMTDiscovery.h"
#include "OMTConstants.h"
#include "OMTLogging.h"

#include <cstring>
#include <cstdio>

// Static storage for discovery addresses (returned to caller)
static std::vector<char*> s_discoveryAddresses;

extern "C" {

// =============================================================
// Discovery API
// =============================================================

char** omt_discovery_getaddresses(int* count) {
    // Free previous results
    for (char* addr : s_discoveryAddresses) {
        free(addr);
    }
    s_discoveryAddresses.clear();
    
    // Get addresses from Discovery
    auto addresses = omt::Discovery::getInstance().getAddresses();
    
    if (addresses.empty()) {
        if (count) *count = 0;
        return nullptr;
    }
    
    // Allocate and copy
    s_discoveryAddresses.reserve(addresses.size());
    for (const auto& addr : addresses) {
        char* copy = static_cast<char*>(malloc(addr.size() + 1));
        strcpy(copy, addr.c_str());
        s_discoveryAddresses.push_back(copy);
    }
    
    if (count) *count = static_cast<int>(s_discoveryAddresses.size());
    return s_discoveryAddresses.data();
}

// Additional Discovery API functions

int omt_discovery_start() {
    return omt::Discovery::getInstance().start() ? 1 : 0;
}

void omt_discovery_stop() {
    omt::Discovery::getInstance().stop();
}

int omt_discovery_register(const char* name, int port) {
    if (!name) return 0;
    return omt::Discovery::getInstance().registerService(name, port) ? 1 : 0;
}

int omt_discovery_unregister(const char* name) {
    if (!name) return 0;
    return omt::Discovery::getInstance().unregisterService(name) ? 1 : 0;
}

// JNI callbacks for Android NsdManager
void omt_discovery_on_service_found(const char* name, const char* host, int port) {
    if (!name) return;
    omt::Discovery::getInstance().onServiceDiscovered(
        name, host ? host : "", port);
}

void omt_discovery_on_service_lost(const char* name) {
    if (!name) return;
    omt::Discovery::getInstance().onServiceLost(name);
}

void omt_discovery_on_registered(const char* name) {
    if (!name) return;
    omt::Discovery::getInstance().onRegistered(name);
}

void omt_discovery_on_registration_failed(const char* name, int errorCode) {
    if (!name) return;
    omt::Discovery::getInstance().onRegistrationFailed(name, errorCode);
}

// =============================================================
// Receive API
// =============================================================

omt_receive_t* omt_receive_create(const char* address, OMTFrameType types,
                                 OMTPreferredVideoFormat format, OMTReceiveFlags flags) {
    try {
        auto receiver = std::make_unique<omt::Receiver>(address, types, format, flags);
        if (!receiver->start()) return nullptr;
        return reinterpret_cast<omt_receive_t*>(receiver.release());
    } catch (...) { return nullptr; }
}
void omt_receive_destroy(omt_receive_t* instance) {
    delete reinterpret_cast<omt::Receiver*>(instance);
}
OMTMediaFrame* omt_receive(omt_receive_t* instance, OMTFrameType types, int timeout) {
    if (!instance) return nullptr;
    try { return reinterpret_cast<omt::Receiver*>(instance)->receive(types, timeout); }
    catch (...) { return nullptr; }
}
int omt_receive_send(omt_receive_t* instance, OMTMediaFrame* frame) {
    if (!instance || !frame) return 0;
    try { return reinterpret_cast<omt::Receiver*>(instance)->sendMetadata(*frame); }
    catch (...) { return 0; }
}
void omt_receive_settally(omt_receive_t* instance, OMTTally* tally) {
    if (instance && tally) reinterpret_cast<omt::Receiver*>(instance)->setTally(*tally);
}
int omt_receive_gettally(omt_receive_t* instance, int timeout, OMTTally* tally) {
    if (!instance || !tally) return 0;
    return reinterpret_cast<omt::Receiver*>(instance)->getTally(timeout, *tally);
}
void omt_receive_setflags(omt_receive_t* instance, OMTReceiveFlags flags) {
    if (instance) reinterpret_cast<omt::Receiver*>(instance)->setFlags(flags);
}
void omt_receive_setsuggestedquality(omt_receive_t* instance, OMTQuality quality) {
    if (instance) reinterpret_cast<omt::Receiver*>(instance)->setQuality(quality);
}
void omt_receive_getsenderinformation(omt_receive_t* instance, OMTSenderInfo* info) {
    if (info) { *info = {}; if (instance) reinterpret_cast<omt::Receiver*>(instance)->senderInfo(*info); }
}
void omt_receive_getvideostatistics(omt_receive_t* instance, OMTStatistics* stats) {
    if (stats) { *stats = {}; if (instance) reinterpret_cast<omt::Receiver*>(instance)->statistics(false,*stats); }
}
void omt_receive_getaudiostatistics(omt_receive_t* instance, OMTStatistics* stats) {
    if (stats) { *stats = {}; if (instance) reinterpret_cast<omt::Receiver*>(instance)->statistics(true,*stats); }
}

// =============================================================
// Send API - Main Implementation
// =============================================================

omt_send_t* omt_send_create(const char* name, OMTQuality quality) {
    LOGI("omt_send_create: name=%s, quality=%d", name ? name : "unnamed", static_cast<int>(quality));
    
    omt::SenderConfig config;
    config.name = name ? name : "OMT Android";
    config.port = 0; // Match libomtnet: allocate a free sender port.
    
    switch (quality) {
        case OMTQuality_Low: config.quality = omt::Quality::Low; break;
        case OMTQuality_Medium: config.quality = omt::Quality::Medium; break;
        case OMTQuality_High: config.quality = omt::Quality::High; break;
        default: config.quality = omt::Quality::Default; break;
    }
    
    auto* sender = new omt::Sender(config);
    if (!sender->start()) {
        LOGE("omt_send_create: failed to start sender");
        delete sender;
        return nullptr;
    }
    
    return reinterpret_cast<omt_send_t*>(sender);
}

void omt_send_destroy(omt_send_t* instance) {
    if (instance) {
        auto* sender = reinterpret_cast<omt::Sender*>(instance);
        delete sender;
    }
}

int omt_send_getport(omt_send_t* instance) {
    return instance ? reinterpret_cast<omt::Sender*>(instance)->port() : 0;
}

int omt_send(omt_send_t* instance, OMTMediaFrame* frame) {
    if (!instance || !frame) return 0;
    
    try {
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    
    if (frame->Type == OMTFrameType_Audio) {
        if (frame->Codec != OMTCodec_FPA1) return 0;
        omt::AudioFrame audio;
        audio.data = frame->Data; audio.dataLength = frame->DataLength;
        audio.sampleRate = frame->SampleRate; audio.channels = frame->Channels;
        audio.samplesPerChannel = frame->SamplesPerChannel; audio.timestamp = frame->Timestamp;
        audio.metadata = frame->FrameMetadata; audio.metadataLength = frame->FrameMetadataLength;
        return sender->sendAudio(audio);
    }
    if (frame->Type != OMTFrameType_Video) return 0;
    
    omt::MediaFrame mf{};
    mf.data = frame->Data;
    mf.dataLength = frame->DataLength;
    mf.width = frame->Width;
    mf.height = frame->Height;
    mf.stride = frame->Stride;
    mf.codec = frame->Codec;
    mf.frameRateN = frame->FrameRateN;
    mf.frameRateD = frame->FrameRateD;
    mf.aspectRatio = frame->AspectRatio;
    mf.flags = frame->Flags;
    mf.colorSpace = frame->ColorSpace;
    mf.timestamp = frame->Timestamp;
    
    return sender->send(mf);
    } catch (...) { return 0; }
}

int omt_send_connections(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->connectionCount();
}

// Custom helper for Android Bridge
void omt_send_set_info_str(omt_send_t* instance, const char* product, const char* manufacturer) {
    if (instance && product && manufacturer) {
        auto* sender = reinterpret_cast<omt::Sender*>(instance);
        sender->setSenderInfo(product, manufacturer);
    }
}

void omt_send_setsenderinformation(omt_send_t* instance, OMTSenderInfo* info) {
    // Legacy struct based implementation - skipped for now
}

void omt_send_addconnectionmetadata(omt_send_t* instance, const char* metadata) {
    if (instance && metadata) {
        auto* sender = reinterpret_cast<omt::Sender*>(instance);
        sender->sendMetadata(metadata);
    }
}

void omt_send_clearconnectionmetadata(omt_send_t* instance) {
    // TODO: implement if needed
}

void omt_send_setredirect(omt_send_t* instance, const char* newAddress) {
    // TODO: implement if needed
}

int omt_send_getaddress(omt_send_t* instance, char* address, int maxLength) {
    return 0;
}

OMTMediaFrame* omt_send_receive(omt_send_t* instance, int timeoutMilliseconds) {
    if(!instance)return nullptr;
    try { return reinterpret_cast<omt::Sender*>(instance)->receiveMetadata(timeoutMilliseconds); }
    catch(...) { return nullptr; }
}

int omt_send_gettally(omt_send_t* instance, int timeoutMilliseconds, OMTTally* tally) {
    if (!instance || !tally) return 0;
    
    return reinterpret_cast<omt::Sender*>(instance)->getTally(timeoutMilliseconds,*tally);
}

void omt_send_getvideostatistics(omt_send_t* instance, OMTStatistics* stats) {
    if (!instance || !stats) return;
    
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    auto s = sender->getStats();
    
    *stats = {};
    stats->Frames = s.totalFramesSent - s.audioFramesSent;
    stats->FramesDropped = s.totalFramesDropped - s.audioFramesDropped;
    stats->BytesSent = s.totalBytesSent - s.audioBytesSent;
}

void omt_send_getaudiostatistics(omt_send_t* instance, OMTStatistics* stats) {
    if (!instance || !stats) return;
    *stats = {};
    const auto s = reinterpret_cast<omt::Sender*>(instance)->getStats();
    stats->Frames = s.audioFramesSent;
    stats->FramesDropped = s.audioFramesDropped;
    stats->BytesSent = s.audioBytesSent;
}

// =============================================================
// Settings API (stubs)
// =============================================================

void omt_setlogcallback(omt_log_callback_t callback, void* userData) {
    omt::logging::setCallback(callback, userData);
}

void omt_setloggingfilename(const char* filename) {}
int omt_settings_get_string(const char* name, char* value, int maxLength) { return 0; }
void omt_settings_set_string(const char* name, const char* value) {}
int omt_settings_get_integer(const char* name) { return 0; }
void omt_settings_set_integer(const char* name, int value) {}

// =============================================================
// Shutdown API
// =============================================================

void omt_shutdown() {
    omt::Discovery::shutdown();
}

// =============================================================
// Extended Statistics API
// =============================================================

int64_t omt_send_get_frames_sent(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getStats().totalFramesSent;
}

int64_t omt_send_get_frames_dropped(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getStats().totalFramesDropped;
}

int64_t omt_send_get_bytes_sent(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getStats().totalBytesSent;
}

int omt_send_get_profile(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getCurrentProfile();
}

void omt_send_set_quality(omt_send_t* instance, OMTQuality quality) {
    if (!instance) return;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    
    omt::Quality q;
    switch (quality) {
        case OMTQuality_Low: q = omt::Quality::Low; break;
        case OMTQuality_Medium: q = omt::Quality::Medium; break;
        case OMTQuality_High: q = omt::Quality::High; break;
        default: q = omt::Quality::Default; break;
    }
    
    sender->setQuality(q);
}

int64_t omt_send_get_recent_drops_and_reset(omt_send_t* instance) {
    // Was a stub returning 0 ("would need to track in Sender"). A consumer that polls
    // this to raise a congestion alert therefore NEVER raised one — the same defect
    // that made the alert permanently unreachable on the Android build. It is now
    // wired to the real per-channel drop windows: Sender::recentDropsAndReset()
    // exchanges each channel's recentDrops_ counter (bumped ONLY inside
    // Channel::countDropped(), so it cannot drift from framesDropped_) and folds in
    // the windows of channels that were destroyed since the last call.
    //
    // Reset semantics: the return value is the number of frames dropped SINCE THE
    // LAST CALL to this function, not a lifetime total — a second call with no
    // further drops answers 0.
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->recentDropsAndReset();
}

} // extern "C"
