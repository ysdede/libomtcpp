/**
 * OMTSender.cpp - OMT Video Sender Implementation
 * Port of libomtnet/src/OMTSend.cs
 */

#include "OMTSender.h"
#include "OMTProtocol.h"
#include "vmxcodec.h"

#include <cstring>
#include <algorithm>
#include <sstream>
#include <string>
#include <limits>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#ifdef _WIN32
  #include "omt_os_win.h"   // full shim LAST: its errno/close macros must not leak into std headers
#endif

#include "OMTLogging.h"
namespace omt {

Sender::Sender(const SenderConfig& config) : config_(config) {
    LOGI("Sender: creating '%s' quality=%d", config_.name.c_str(), static_cast<int>(config_.quality));
    currentProfile_ = selectProfile(config_.quality);
}

Sender::~Sender() {
    LOGI("Sender: destroying");
    stop();
    destroyEncoder();
}

bool Sender::start() {
    // P0 #5 (Codex): serialize the lifecycle so start/stop cannot interleave.
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (running_.load()) return true;
    
    const int ss = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ss < 0) {
        LOGE("Sender: failed to create socket errno=%d (%s)", errno, std::strerror(errno));
        return false;
    }
    serverSocket_.store(ss);
    
    int one = 1;
    setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // Phase C: bind to the configured local IPv4. Empty / "0.0.0.0" / "auto"
    // means all interfaces (INADDR_ANY). Any other value must parse as a dotted
    // quad or we fail-closed (visible) — silently falling back to all interfaces
    // would defeat the point of choosing an adapter on a multi-NIC host.
    const std::string& bindAddr = config_.bindAddress;
    if (bindAddr.empty() || bindAddr == "0.0.0.0" || bindAddr == "auto") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
        LOGE("Sender: invalid bind address '%s' (errno=%d %s) — refusing to start",
             bindAddr.c_str(), errno, std::strerror(errno));
        close(ss);
        serverSocket_ = -1;
        return false;
    }
    // P2 #8 (Codex): validate the port at the ACTUAL narrowing boundary — config_.port
    // is an int fed to htons (uint16_t); out-of-range values wrap silently (65536->0,
    // -1->65535) and bind the wrong port. Fail-closed, matching the bind-address guard.
    if (config_.port < 1 || config_.port > 65535) {
        LOGE("Sender: listen port %d out of range 1..65535 — refusing to start", config_.port);
        close(ss);
        serverSocket_ = -1;
        return false;
    }
    addr.sin_port = htons(config_.port);
    
    if (bind(ss, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("Sender: failed to bind %s:%d errno=%d (%s)",
             bindAddr.empty() ? "0.0.0.0" : bindAddr.c_str(), config_.port, errno, std::strerror(errno));
        close(ss);
        serverSocket_ = -1;
        return false;
    }
    
    // P0 #5 (Codex): check listen() — a silent failure would leave start()
    // reporting success with a non-listening socket.
    if (listen(ss, 5) != 0) {
        LOGE("Sender: listen() failed errno=%d", errno);
        close(ss);
        serverSocket_ = -1;
        return false;
    }
    running_ = true;
    
    LOGI("Sender: listening on port %d", config_.port);
    
    acceptThread_ = std::thread(&Sender::acceptLoop, this);
    return true;
}

void Sender::stop() {
    // P0 #5 (Codex): serialize the lifecycle (matches start()).
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (!running_.load()) {
        // Not running, but a stale listener fd may still be open (e.g. a failed
        // start after bind/listen). Close it exactly once and return.
        const int stale = serverSocket_.exchange(-1);
        if (stale >= 0) ::close(stale);
        return;
    }
    running_ = false;

    // P0 #5 (Codex): shutdown the listener to unblock a blocked accept() WITHOUT
    // closing it. On Linux shutdown(SHUT_RDWR) on a listening socket makes the
    // pending accept() return EINVAL, which acceptLoop breaks on. We must NOT
    // close() before the accept thread is joined: closing frees the fd number,
    // which another thread's socket()/accept() could recycle, so the accept
    // thread would then accept() on a descriptor it no longer owns. Order is
    // shutdown -> join -> close (the channel's shutdown/join/close is the model).
#ifdef _WIN32
    // Windows: shutdown() is defined only for connected sockets and does NOT wake
    // a pending accept() — the Linux idiom would hang the join() below until some
    // client connects. closesocket() cancels the blocked accept (it returns
    // WSAENOTCONN/WSAEINVAL), and acceptLoop then falls out through its
    // running_ checks. Handled via exchange so the join-time close below is a
    // no-op and the fd is closed exactly once.
    const int waking = serverSocket_.exchange(-1);
    if (waking >= 0) ::close(waking);   // full shim maps close -> closesocket
#else
    const int lfd = serverSocket_.load();
    if (lfd >= 0) ::shutdown(lfd, SHUT_RDWR);
#endif

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // The accept thread is gone — nobody touches the listener fd now, so closing
    // it here (exactly once, via exchange) is free of the descriptor-reuse race.
    const int closing = serverSocket_.exchange(-1);
    if (closing >= 0) ::close(closing);
    
    // Disconnect all channels. P0 #4 (Codex): swap the vector OUT under the lock,
    // then destroy it OUTSIDE the lock. ~Channel joins its receiver thread, and
    // that receiver can be blocked in a callback (onDisconnected /
    // onQualityChanged -> getBestSuggestedQuality) waiting for channelsMutex_.
    // Destroying while holding it deadlocks. Never destroy the sender
    // synchronously from its own accept/receiver callbacks.
    std::vector<std::unique_ptr<Channel>> retiring;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        retiring.swap(channels_);
    }
    retiring.clear(); // joins channel threads outside channelsMutex_
    
    LOGI("Sender: stopped");
}

void Sender::acceptLoop() {
    LOGI("acceptLoop: started");
    
    while (running_) {
        const int lfd = serverSocket_.load();
        if (lfd < 0) break;  // stop() closed the listener
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        
        int clientFd = accept(lfd, 
                              reinterpret_cast<sockaddr*>(&clientAddr), 
                              &addrLen);
        
        if (clientFd < 0) {
            if (errno == EINVAL || errno == EBADF) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // P0 #5 (Codex): stop() may have begun while we were blocked in accept().
        // Do not publish a channel into a sender that is shutting down.
        if (!running_.load()) {
            ::close(clientFd);
            break;
        }
        
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        LOGI("New connection from %s:%d", clientIP, ntohs(clientAddr.sin_port));
        
        // Get current optimal buffer size
        int bufferSize = config_.sendBufferSize;
        {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (currentWidth_ > 0 && currentHeight_ > 0) {
                bufferSize = calculateOptimalBuffer(currentWidth_, currentHeight_, currentProfile_);
            }
        }
        
        auto channel = std::make_unique<Channel>(clientFd, clientAddr, bufferSize, this,
                                                 config_.sendQueueSize, config_.budgets);
        
        // Send configured sender info immediately.
        //
        // QUEUED, not written here. The banner used to go out through the synchronous
        // writer on the accept thread, which had two measured consequences: (1) the
        // accept loop sat in a 5-6 second bounded write whenever the new viewer was
        // not draining yet, holding up every OTHER incoming connection; (2) when that
        // write did not finish, the channel was called dead — 802 times in three
        // minutes against OBS, because a viewer that has not read yet is exactly what
        // a connecting viewer looks like. queueMetadata() puts the banner in the
        // channel's single waiting slot and lets its own writer thread send it the
        // moment the socket will take it, so the banner still reaches the peer (proved
        // by qa-ompin/wire-slow-reader.cpp) without the accept thread ever blocking.
        //
        // P0 #8 (Codex): snapshot config_.senderInfoXml under senderInfoMutex_ so
        // the handshake read never races setSenderInfo() on the std::string.
        std::string infoXml;
        {
            std::lock_guard<std::mutex> lk(senderInfoMutex_);
            infoXml = config_.senderInfoXml;
        }
        channel->queueMetadata(infoXml.c_str());

        if (onConnect_) {
            onConnect_(channel.get());
        }
        
        addChannel(std::move(channel));
    }
    
    LOGI("acceptLoop: exiting");
}

void Sender::addChannel(std::unique_ptr<Channel> channel) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    channels_.push_back(std::move(channel));
}

void Sender::removeChannel(Channel* channel) {
    // P0 #4 (Codex): move the retiring channel OUT under the lock, destroy it
    // (join its threads) OUTSIDE the lock — ~Channel joins the receiver, which
    // may be blocked in a callback waiting for channelsMutex_.
    std::unique_ptr<Channel> retiring;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (auto it = channels_.begin(); it != channels_.end(); ++it) {
            if (it->get() == channel) {
                // Bank its un-read window BEFORE it dies — and drain the queue first,
                // because ~Channel's own discardPending() counts those frames as drops
                // and by then there is no channel left to bank them from.
                retiring = std::move(*it);
                retiring->discardPending();     // counts its own drops into the window
                retiredRecentDrops_ += retiring->takeRecentDrops();
                channels_.erase(it);
                break;
            }
        }
    }
}

namespace {

// Scratch-buffer policy shared by the encode paths: grown to `need` without
// reallocating in steady state; the size is NEVER shrunk after a frame,
// because a later grow-back would value-initialize (memset) the whole block —
// measured as a 2x send() regression at 4K in the zero-copy draft this
// distilled from. Consumers take data() + explicit length, so the trailing
// bytes past the published frame are simply dead until overwritten. Memory is
// RELEASED only when the requirement has fallen far below the retained block
// (a resolution drop): 4x + 8 MiB hysteresis prevents thrashing.
void ensureScratch(std::vector<uint8_t>& buf, size_t need) {
    if (buf.capacity() < need) {
        buf.reserve(need);
    } else if (buf.capacity() >= need * 4u && buf.capacity() - need >= (size_t(8) << 20)) {
        std::vector<uint8_t> fresh;
        fresh.reserve(need);
        buf.swap(fresh);   // one-time; the single resize below re-zeroes once
    }
    if (buf.size() < need) buf.resize(need);
}

} // namespace

int Sender::cleanupDisconnected() {
    // P0 #4 (Codex): collect disconnected channels into a local owning vector
    // under the lock, then destroy (join) them after releasing it. Returns the
    // number of channels still connected, so callers retire channels and test
    // for subscribers in ONE channelsMutex_ pass.
    std::vector<std::unique_ptr<Channel>> retiring;
    int activeCount = 0;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (auto it = channels_.begin(); it != channels_.end(); ) {
            Channel* c = it->get();
            if (c && !c->isConnected()) {
                // Same banking as removeChannel(): the window outlives the channel,
                // including the frames ~Channel is about to throw away below. The drain
                // bumps the channel's own counter, so it must run BEFORE the take —
                // adding its return value as well would report those frames twice.
                c->discardPending();
                retiredRecentDrops_ += c->takeRecentDrops();
                retiring.push_back(std::move(*it));
                it = channels_.erase(it);
            } else {
                ++it;
            }
        }
        activeCount = static_cast<int>(channels_.size());
    }
    return activeCount;
}

int Sender::connectionCount() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    return static_cast<int>(channels_.size());
}

int Sender::videoSubscriberCount() const {
    // P0 #20 (Codex): only connected channels that actually subscribed to VIDEO
    // count as subscribers — a raw TCP connection (or an audio/metadata-only
    // peer) is not a video subscriber.
    std::lock_guard<std::mutex> lock(channelsMutex_);
    int n = 0;
    for (const auto& ch : channels_)
        if (ch->isConnected() && ch->isVideoSubscribed()) ++n;
    return n;
}

SenderStats Sender::getStats() {
    cleanupDisconnected();
    
    SenderStats stats;
    stats.totalEncodedBytes = totalEncodedBytes_.load();
    
    std::lock_guard<std::mutex> lock(channelsMutex_);
    stats.connectionCount = static_cast<int>(channels_.size());
    
    for (const auto& ch : channels_) {
        auto chStats = ch->getStats();
        stats.totalFramesSent += chStats.framesSent;
        stats.totalFramesDropped += chStats.framesDropped;
        stats.totalBytesSent += chStats.bytesSent;
        stats.totalFramesOffered += chStats.framesOffered;
        stats.totalFramesTorn += chStats.framesTorn;
        stats.totalStaleEvicted += ch->staleEvicted();
    }
    
    return stats;
}

int64_t Sender::recentDropsAndReset() {
    // Take the retired windows FIRST, then the live channels'. A channel retired in
    // between lands in the NEXT call's window rather than being double-counted here —
    // the hand-off is an exchange on each channel's own counter, so a window can be
    // read by exactly one caller either way.
    int64_t n = retiredRecentDrops_.exchange(0);
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_)
        n += ch->takeRecentDrops();
    return n;
}

int Sender::selectProfile(Quality quality) const {
    switch (quality) {
        case Quality::Low: return VMX_PROFILE_OMT_LQ;
        case Quality::Medium: return VMX_PROFILE_OMT_SQ;
        case Quality::High: return VMX_PROFILE_OMT_HQ;
        default: return VMX_PROFILE_OMT_SQ;
    }
}

void Sender::setQuality(Quality quality) {
    LOGI("Updating quality to %d", static_cast<int>(quality));
    
    std::lock_guard<std::mutex> lock(encoderMutex_);
    config_.quality = quality;
    
    if (quality != Quality::Default) {
        int newProfile = selectProfile(quality);
        if (newProfile != currentProfile_) {
            currentProfile_ = newProfile;
            destroyEncoder();
            LOGI("Profile changed to %d on manual request", newProfile);
        }
    }
}

int Sender::calculateOptimalBuffer(int width, int height, int profile) {
    int rawSize = width * height * 3 / 2;
    
    float compressionRatio;
    switch (profile) {
        case VMX_PROFILE_OMT_HQ: compressionRatio = 0.25f; break;
        case VMX_PROFILE_OMT_SQ: compressionRatio = 0.18f; break;
        case VMX_PROFILE_OMT_LQ: compressionRatio = 0.12f; break;
        default: compressionRatio = 0.20f; break;
    }
    
    int bufferSize = static_cast<int>(rawSize * compressionRatio * 1.5f) + 4096;
    
    if (bufferSize < 256 * 1024) bufferSize = 256 * 1024;
    if (bufferSize > 2 * 1024 * 1024) bufferSize = 2 * 1024 * 1024;
    
    return bufferSize;
}

void Sender::createEncoder(int width, int height, int frameRate, int colorSpace) {
    std::lock_guard<std::mutex> lock(encoderMutex_);
    
    if (vmxInstance_ && currentWidth_ == width && currentHeight_ == height) {
        return;  // Already have correct encoder. Recreated via destroyEncoder() if profile changed.
    }
    
    destroyEncoder();
    
    VMX_SIZE size = {width, height};
    VMX_COLORSPACE cspace = static_cast<VMX_COLORSPACE>(colorSpace);
    
    vmxInstance_ = VMX_Create(size, static_cast<VMX_PROFILE>(currentProfile_), cspace);
    currentWidth_ = width;
    currentHeight_ = height;
    
    LOGI("Encoder created: %dx%d, profile=%d", width, height, static_cast<int>(currentProfile_));
}

void Sender::destroyEncoder() {
    if (vmxInstance_) {
        VMX_Destroy(vmxInstance_);
        vmxInstance_ = nullptr;
    }
    currentWidth_ = 0;
    currentHeight_ = 0;
}

int Sender::encodeFrame(const MediaFrame& frame) {
    std::lock_guard<std::mutex> lock(encoderMutex_);
    
    if (!vmxInstance_) return 0;
    
    int interlaced = (frame.flags & VideoFlags::Interlaced) ? 1 : 0;
    // Defense-in-depth (Codex interlace review): keep this legacy/alternate encode path
    // consistent with encodeFrameLocked — never advertise the Interlaced wire flag on a
    // height VMX encodes as progressive (480/576/1080 only). This overload is currently
    // unreferenced by the node, but guarding it prevents a lying header if it is revived.
    if (interlaced && !(frame.height == 480 || frame.height == 576 || frame.height == 1080)) {
        LOGE("interlaced at unsupported height %d (VMX interlaces only 480/576/1080) — refusing",
             frame.height);
        return 0;
    }
    VMX_ERR err = VMX_ERR_UNKNOWN;
    
    BYTE* data = static_cast<BYTE*>(frame.data);
    
    // Encode based on input codec
    switch (frame.codec) {
        case Codec::UYVY:
            err = VMX_EncodeUYVY(vmxInstance_, data, frame.stride, interlaced);
            break;
        case Codec::YUY2:
            err = VMX_EncodeYUY2(vmxInstance_, data, frame.stride, interlaced);
            break;
        case 0x3231564E:  // NV12
            {
                BYTE* y = data;
                BYTE* uv = data + (frame.stride * frame.height);
                err = VMX_EncodeNV12(vmxInstance_, y, frame.stride, uv, frame.stride, interlaced);
            }
            break;
        case Codec::BGRA:
            err = VMX_EncodeBGRA(vmxInstance_, data, frame.stride, interlaced);
            break;
        default:
            LOGE("Unknown codec: 0x%08X", frame.codec);
            return 0;
    }
    
    if (err != VMX_ERR_OK) {
        LOGE("VMX encode failed: %d", err);
        return 0;
    }
    
    // Encode into a reused thread-local scratch (same policy as the locked
    // path; uint64 size math kept consistent with it). This legacy overload is
    // currently unreferenced by the node but stays allocation-free too.
    thread_local std::vector<uint8_t> scratch;
    uint64_t maxSize64 = (uint64_t)frame.width * (uint64_t)frame.height * 4u;
    if (maxSize64 < (1024u * 1024u)) maxSize64 = 1024u * 1024u;
    if (maxSize64 > (uint64_t)std::numeric_limits<size_t>::max()) return 0;
    ensureScratch(scratch, (size_t)maxSize64);
    return VMX_SaveTo(vmxInstance_, scratch.data(), (int)scratch.size());
}

// P0 #9 (Codex): reject out-of-range dimensions/stride/length BEFORE any
// allocation or VMX access. Uses checked uint64 arithmetic; VMX's real limits
// are width 16..7680, height 16..4320, even width.
bool Sender::validateFrame(const MediaFrame& frame) {
    if (frame.data == nullptr) return false;
    if (frame.width < 16 || frame.width > 7680) return false;
    if (frame.height < 16 || frame.height > 4320) return false;
    if ((frame.width & 1) != 0) return false;                 // VMX needs even width
    if (frame.stride <= 0) return false;
    if (frame.frameRateD <= 0) return false;                   // P0 #9 (Codex final): positive denominator too (send() divides by it; a negative value is invalid)
    if (frame.frameRateN <= 0) return false;                   // P0 #16: positive rate
    if (frame.dataLength < 0) return false;
    // P0 #9 (Codex): the required byte count is CODEC-SPECIFIC. The previous
    // version applied BGRA's width*4 row size to every codec, so it rejected
    // valid tightly-packed UYVY/NV12 and could accept a truncated padded NV12.
    // Compute `need` per codec and require stride >= the codec's row width.
    const uint64_t h = static_cast<uint64_t>(frame.height);
    const uint64_t s = static_cast<uint64_t>(frame.stride);
    const uint64_t w = static_cast<uint64_t>(frame.width);
    uint64_t need = 0;
    if (frame.codec == Codec::BGRA || frame.codec == Codec::BGRX) {
        const uint64_t rowBytes = w * 4u;
        if (s < rowBytes) return false;
        need = (h - 1) * s + rowBytes;
    } else if (frame.codec == Codec::UYVY || frame.codec == Codec::YUY2) {
        const uint64_t rowBytes = w * 2u;                      // packed 4:2:2
        if (s < rowBytes) return false;
        need = (h - 1) * s + rowBytes;
    } else if (frame.codec == Codec::NV12) {
        if (s < w) return false;                               // Y row holds width bytes
        // P0 #9 (Codex final): NV12 subsamples chroma vertically (height>>1 chroma
        // rows), so an odd height leaves the final luma row without chroma. Require
        // an even height for NV12 specifically (width evenness is checked above).
        if ((frame.height & 1) != 0) return false;
        // Y plane (h rows * stride) + interleaved UV plane (h/2 rows * stride).
        need = h * s + (h / 2u) * s;
    } else {
        return false;                                          // unknown codec
    }
    if (static_cast<uint64_t>(frame.dataLength) < need) return false;
    // Codex sendfeatures P1: the ext-header carries frame.colorSpace VERBATIM
    // (OMTSender.cpp:592), while VMX's BGRA/BGRX dispatch only ever selects a 601 or
    // 709 conversion table (vmxcodec.cpp:1432/1638 — the 2020 case is not wired for
    // these inputs). Accepting 2020 here therefore produced a payload encoded with the
    // 709 matrix under a header advertising BT.2020 primaries: a lie that a loopback
    // between two copies of the same fallback hides, and that a downstream colour-managed
    // consumer will faithfully mis-render. The established contract at this boundary is
    // undefined(0)/601/709; anything else is refused BEFORE the encoder is touched.
    // Native 2020 needs validated forward/inverse tables plus a qualified receiver path,
    // not a different enum value here.
    if (!(frame.colorSpace == ColorSpace::Unknown || frame.colorSpace == ColorSpace::BT601 ||
          frame.colorSpace == ColorSpace::BT709)) {
        LOGE("send: refusing colour space %d — VMX encodes 601/709 matrices only; advertising "
             "%d on the wire over a 709-matrix payload would be a lying header",
             frame.colorSpace, frame.colorSpace);
        return false;
    }
    return true;
}

// P0 #8 (Codex): create-or-reuse the encoder for (w,h,colorSpace,profile).
// Caller MUST hold encoderMutex_.
bool Sender::ensureEncoderLocked(int width, int height, int colorSpace) {
    if (vmxInstance_ && currentWidth_ == width && currentHeight_ == height &&
        currentColorSpace_ == colorSpace) {
        return true;
    }
    destroyEncoder();
    VMX_SIZE size = {width, height};
    VMX_COLORSPACE cspace = static_cast<VMX_COLORSPACE>(colorSpace);
    vmxInstance_ = VMX_Create(size, static_cast<VMX_PROFILE>(currentProfile_), cspace);
    if (!vmxInstance_) {
        LOGE("VMX_Create failed for %dx%d cs=%d", width, height, colorSpace);
        return false;
    }
    currentWidth_ = width;
    currentHeight_ = height;
    currentColorSpace_ = colorSpace;
    LOGI("Encoder created: %dx%d cs=%d profile=%d", width, height, colorSpace, currentProfile_);
    return true;
}

// P0 #8 (Codex): encode under the (already-held) encoderMutex_ DIRECTLY into
// `out`, the caller-owned scratch, so there is no intermediate buffer and no
// second copy of the compressed frame. `out` is only read by the caller on
// this thread; Channel::sendAsync copies bytes into pooled buffers before it
// returns, so reuse of the scratch cannot overlap an in-flight frame.
// Returns encoded length (0 = failure).
int Sender::encodeFrameLocked(const MediaFrame& frame, std::vector<uint8_t>& out) {
    if (!vmxInstance_) return 0;
    int interlaced = (frame.flags & VideoFlags::Interlaced) ? 1 : 0;
    // Defense-in-depth (Codex interlace review): VMX only encodes interlaced at
    // 480/576/1080 (VMX_ConfigureInterlaced); at any other height it silently stays
    // PROGRESSIVE, so advertising the Interlaced wire flag would make the header lie
    // about the payload. Fail closed HERE too — the node's height gate normally rejects
    // this first with a user-facing status, but a direct sender.send must never emit an
    // interlaced-flagged progressive payload (this is the path the native test exposed).
    if (interlaced && !(frame.height == 480 || frame.height == 576 || frame.height == 1080)) {
        LOGE("interlaced at unsupported height %d (VMX interlaces only 480/576/1080) — "
             "refusing to advertise a lying header", frame.height);
        return 0;
    }
    VMX_ERR err = VMX_ERR_UNKNOWN;
    BYTE* data = static_cast<BYTE*>(frame.data);
    switch (frame.codec) {
        case Codec::UYVY: err = VMX_EncodeUYVY(vmxInstance_, data, frame.stride, interlaced); break;
        case Codec::YUY2: err = VMX_EncodeYUY2(vmxInstance_, data, frame.stride, interlaced); break;
        case 0x3231564E: { // NV12
            BYTE* y = data;
            BYTE* uv = data + (frame.stride * frame.height);
            err = VMX_EncodeNV12(vmxInstance_, y, frame.stride, uv, frame.stride, interlaced);
        } break;
        case Codec::BGRA: err = VMX_EncodeBGRA(vmxInstance_, data, frame.stride, interlaced); break;
        case Codec::BGRX: err = VMX_EncodeBGRX(vmxInstance_, data, frame.stride, interlaced); break;
        default: LOGE("Unknown codec: 0x%08X", frame.codec); return 0;
    }
    if (err != VMX_ERR_OK) { LOGE("VMX encode failed: %d", err); return 0; }
    uint64_t maxSize64 = (uint64_t)frame.width * (uint64_t)frame.height * 4u;
    if (maxSize64 < (1024u * 1024u)) maxSize64 = 1024u * 1024u;
    if (maxSize64 > (uint64_t)std::numeric_limits<size_t>::max()) return 0;
    ensureScratch(out, (size_t)maxSize64);
    int encodedLen = VMX_SaveTo(vmxInstance_, out.data(), (int)out.size());
    // P0 #10 (Codex): a capacity/arg failure returns 0 or a bogus positive; a
    // publishable frame must be >0 and within the buffer we actually own.
    if (encodedLen <= 0 || (size_t)encodedLen > out.size()) {
        LOGE("VMX_SaveTo bad length %d (cap %zu)", encodedLen, out.size());
        return 0;
    }
    // Deliberately NOT out.resize(encodedLen): shrinking the size would make
    // the next frame's grow-back zero the whole scratch (see ensureScratch).
    // Callers consume data() + encodedLen.
    return encodedLen;
}

int Sender::send(const MediaFrame& frame) {
    // Retire disconnected channels and test for subscribers in one lock pass.
    if (cleanupDisconnected() == 0) return 1;  // No clients, but not an error
    
    // P0 #9: validate input before touching the encoder or VMX.
    if (!validateFrame(frame)) {
        LOGE("send: rejected invalid frame %dx%d stride=%d len=%d rate=%d/%d",
             frame.width, frame.height, frame.stride, frame.dataLength,
             frame.frameRateN, frame.frameRateD);
        return 0;
    }
    
    // P0 #8: encoder-select + encode + payload-copy as ONE transaction under a
    // single encoderMutex_ hold; the payload is owned locally so a concurrent
    // quality callback (destroyEncoder) or second producer cannot mix
    // dimensions/headers/payload. Congestion/quality decisions run AFTER.
    thread_local std::vector<uint8_t> payload;   // reused scratch; see sendAsync note
    int encodedLen = 0;
    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        if (!ensureEncoderLocked(frame.width, frame.height, frame.colorSpace))
            return 0;
        encodedLen = encodeFrameLocked(frame, payload);
    }
    if (encodedLen <= 0) return 0;
    
    totalEncodedBytes_ += encodedLen;
    int count = frameCount_++;
    
    // Log periodically
    if (count % 30 == 0) {
        float avgBytes = static_cast<float>(totalEncodedBytes_.load()) / (count + 1);
        float mbps = (avgBytes * 8.0f * frame.frameRateN / frame.frameRateD) / 1000000.0f;
        LOGI("Frame %d: %d bytes (~%.1f Mbps)", count, encodedLen, mbps);
        checkCongestion();
    }
    
    // Build headers
    FrameHeader header{};
    header.version = 1;
    header.frameType = static_cast<uint8_t>(FrameType::Video);
    header.timestamp = frame.timestamp;
    header.metadataLength = 0;
    header.dataLength = VideoExtHeader::SIZE + encodedLen;
    
    VideoExtHeader extHeader{};
    extHeader.codec = Codec::VMX1;
    extHeader.width = frame.width;
    extHeader.height = frame.height;
    extHeader.frameRateN = frame.frameRateN;
    extHeader.frameRateD = frame.frameRateD;
    extHeader.aspectRatio = frame.aspectRatio;
    extHeader.flags = frame.flags;
    extHeader.colorSpace = frame.colorSpace;
    
    // Send to all subscribed clients. The payload is this thread's reused
    // scratch: safe because sendAsync copies into pooled buffers before
    // returning, so the next frame's overwrite cannot touch an in-flight one.
    int totalSent = 0;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (const auto& ch : channels_) {
            if (!ch->isConnected()) continue;
            
            // Only send if client has subscribed to video (per OMT protocol)
            if (!ch->isVideoSubscribed()) continue;
            
            int sent = ch->sendAsync(&header, sizeof(header),
                                     &extHeader, sizeof(extHeader),
                                     payload.data(), encodedLen);
            if (sent > 0) {
                totalSent += sent;
            }
        }
    }
    
    return totalSent > 0 ? encodedLen : 0;
}

void Sender::sendMetadata(const std::string& xml) {
    // P0 #7 (Codex): the per-channel write (sendMetadataSync) is BOUNDED but not
    // cheap: acquisition polls try_lock() up to ~6s, then sendAllPoll has its own
    // ~5s write deadline — so worst case is ~11s PER CHANNEL, and the broadcast
    // scales with the connected-channel count (no fixed overall deadline). This is
    // an accepted latency tradeoff: retaining channelsMutex_ across the bounded
    // write avoids the use-after-free a raw-pointer snapshot would introduce, and a
    // shared_ptr<Channel> model + in-flight-broadcast barrier (to keep ~Channel's
    // thread-join from being deferred past ~Sender) is the only way to remove the
    // hold entirely — deferred. The corrupt-stream and unbounded-wait defects are
    // closed here.
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected()) {
            ch->sendMetadataSync(xml.c_str());
        }
    }
}

Quality Sender::getBestSuggestedQuality() const {
    Quality best = Quality::Default;
    
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected()) {
            Quality q = ch->getSuggestedQuality();
            if (static_cast<int>(q) > static_cast<int>(best)) {
                best = q;
            }
        }
    }
    
    return best;
}

bool Sender::hasTallyPreview() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected() && ch->isTallyPreview()) {
            return true;
        }
    }
    return false;
}

bool Sender::hasTallyProgram() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected() && ch->isTallyProgram()) {
            return true;
        }
    }
    return false;
}

// IChannelListener implementation

void Sender::onDisconnected(Channel* channel) {
    LOGI("Channel disconnected");
    if (onDisconnect_) {
        onDisconnect_(channel);
    }
    // Cleanup will happen on next send or getStats call
}

void Sender::onTallyChanged(Channel* channel) {
    LOGI("Tally changed: preview=%d, program=%d",
         channel->isTallyPreview(), channel->isTallyProgram());
}

void Sender::onQualityChanged(Channel* channel) {
    LOGI("Quality suggested: %d", static_cast<int>(channel->getSuggestedQuality()));
    // P0 #8 (Codex): gather the aggregated suggestion FIRST — getBestSuggestedQuality
    // locks channelsMutex_ internally, so we must NOT hold encoderMutex_ while
    // acquiring it (lock-order discipline: never encoderMutex_ -> channelsMutex_).
    Quality best = getBestSuggestedQuality();
    if (best == Quality::Default) return;
    // Then take the encoder lock and RECHECK auto-quality before applying.
    // config_.quality is written by setQuality() under encoderMutex_, so reading
    // it here (was: at the top of this function, outside the lock) is the fix for
    // the data race Codex flagged.
    std::lock_guard<std::mutex> lock(encoderMutex_);
    if (config_.quality != Quality::Default) return; // not auto anymore
    int newProfile = selectProfile(best);
    if (newProfile != currentProfile_) {
        currentProfile_ = newProfile;
        // Encoder will be recreated on next send
        destroyEncoder();
        LOGI("Profile changed to %d based on receiver suggestion",
             static_cast<int>(newProfile));
    }
}

void Sender::setSenderInfo(const std::string& productName, const std::string& manufacturer) {
    std::ostringstream ss;
    ss << "<OMTInfo ProductName=\"" << productName << "\" Manufacturer=\"" << manufacturer << "\" Version=\"1.0\" />";
    std::string xml = ss.str();
    // P0 #8 (Codex): config_.senderInfoXml is read on the accept-loop handshake;
    // publish it under senderInfoMutex_ so the two never race on the std::string.
    {
        std::lock_guard<std::mutex> lock(senderInfoMutex_);
        config_.senderInfoXml = xml;
    }
    // Broadcast to existing channels using the local snapshot (do not re-read
    // config_ while holding the socket write).
    sendMetadata(xml);
}

// P0 #8 (Codex round-5/6): the FULL congestion decision — threshold + quality gate +
// downgrade map — factored into a pure function so the native harness can DIRECTLY
// observe congestion-adjustment execution (the flaky network-backpressure path cannot
// reliably clear the 5% threshold in a unit run). checkCongestion() calls it UNDER
// encoderMutex_ so config_.quality (written by setQuality under the same lock) and
// currentProfile_ are read/written inside the critical section.
int Sender::congestionDecision(double dropRate, Quality quality, int currentProfile) {
    if (dropRate <= 0.05) return currentProfile;                                  // below threshold: no action
    if (!(quality == Quality::Default || quality == Quality::High || quality == Quality::Medium))
        return currentProfile;                                                     // manual Low pins the profile
    if (currentProfile == VMX_PROFILE_OMT_HQ) return VMX_PROFILE_OMT_SQ;          // HQ -> SQ
    if (currentProfile == VMX_PROFILE_OMT_SQ) return VMX_PROFILE_OMT_LQ;          // SQ -> LQ
    return currentProfile;                                                        // already LQ / unknown
}

void Sender::applyCongestionLocked(double dropRate) {
    // P0 #8 (Codex): acquire encoderMutex_ BEFORE reading config_.quality / writing
    // currentProfile_ — both are shared with setQuality(). getStats() reads only channel
    // counters (channelsMutex_), so computing dropRate in checkCongestion does not touch
    // that state. The harness drives THIS real locked state-update directly (via
    // applyCongestionForTest) concurrently with setQuality to prove the concurrency.
    std::lock_guard<std::mutex> lock(encoderMutex_);
    const int current = currentProfile_;
    const int lower = congestionDecision(dropRate, config_.quality, current);
    if (lower != current) {
        LOGW("High drop rate %.2f%%: congestion downgraded profile %d -> %d", dropRate * 100.0, current, lower);
        currentProfile_ = lower;
        destroyEncoder();
    }
}

void Sender::checkCongestion() {
    SenderStats stats = getStats();
    if (stats.totalFramesSent < 30) return; // Too early
    const double dropRate = (double)stats.totalFramesDropped / (double)(stats.totalFramesSent + stats.totalFramesDropped);
    applyCongestionLocked(dropRate);
}

// ... existing code ...

} // namespace omt
