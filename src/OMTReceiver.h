// Copyright (c) 2026 Zero Density. Licensed under the MIT License.
#pragma once
#include "libomt.h"
#include <memory>

namespace omt {
class Receiver {
public:
    Receiver(const char* address, OMTFrameType types, OMTPreferredVideoFormat format, OMTReceiveFlags flags);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    bool start();
    OMTMediaFrame* receive(OMTFrameType types, int timeoutMs);
    int sendMetadata(const OMTMediaFrame& frame);
    void setTally(const OMTTally& tally);
    int getTally(int timeoutMs, OMTTally& tally);
    void setFlags(OMTReceiveFlags flags);
    void setQuality(OMTQuality quality);
    void senderInfo(OMTSenderInfo& info);
    void statistics(bool audio, OMTStatistics& stats);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
