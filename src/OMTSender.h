/**
 * OMTSender.h - OMT Video Sender
 * Port of libomtnet/src/OMTSend.cs
 * 
 * Main sender class that:
 * - Accepts TCP connections
 * - Manages channels (clients)
 * - Encodes video with VMX
 * - Sends frames to subscribers
 */

#ifndef OMT_SENDER_H
#define OMT_SENDER_H

#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <functional>

#include "OMTConstants.h"
#include "OMTChannel.h"

// Forward declare VMX instance only (struct is fine)
struct VMX_INSTANCE;

namespace omt {

/**
 * Sender statistics
 */
struct SenderStats {
    int64_t totalFramesSent = 0;
    int64_t totalFramesDropped = 0;
    int64_t totalBytesSent = 0;
    int64_t totalEncodedBytes = 0;
    int connectionCount = 0;
    // Aggregated per-channel frame partition (see ChannelStats): every frame handed to
    // a channel is offered once and lands in exactly one terminal counter, so
    //   totalFramesOffered == sent + dropped + torn   (at quiescence)
    // is checkable from the outside. totalStaleEvicted is the subset of the drops the
    // drop-oldest policy itself caused.
    int64_t totalFramesOffered = 0;
    int64_t totalFramesTorn = 0;
    int64_t totalStaleEvicted = 0;
};

/**
 * Video frame information (matches libomt.h OMTMediaFrame)
 */
struct MediaFrame {
    void* data;
    int dataLength;
    int width;
    int height;
    int stride;
    int codec;
    int frameRateN;
    int frameRateD;
    float aspectRatio;
    int flags;
    int colorSpace;
    int64_t timestamp;
};

// P0 #3 / #14 (Codex): the SINGLE production mapping from the node's captured
// frame state to the on-wire video fields. Factored out of the worker so it is
// unit-testable (qa-omp/review-native.cpp drives it through Sender::send and
// independently parses the serialized VideoExtHeader) — proving the opaque vs
// straight decision actually reaches the wire, not merely that a hand-built
// struct round-trips the flags it set itself. The node's worker calls THIS.
inline void applyVideoFrameMapping(MediaFrame& mf, bool opaque, bool interlaced,
                                   int wire_colorspace) {
    mf.codec = opaque ? Codec::BGRX : Codec::BGRA;
    mf.flags = (opaque ? 0 : VideoFlags::Alpha) |
               (interlaced ? VideoFlags::Interlaced : 0);
    mf.colorSpace = wire_colorspace;
}

/**
 * Sender configuration
 */
struct SenderConfig {
    std::string name = "OMT Sender";
    Quality quality = Quality::Default;
    // Zero selects the first available port in [portStart, portEnd].
    // A nonzero port is exact: a conflict never silently changes the endpoint.
    int port = 0;
    // Phase C: which local IPv4 address to bind the TCP listener to. "0.0.0.0"
    // (the default, and the canonical value for "auto") binds all interfaces
    // (INADDR_ANY); an explicit dotted-quad binds ONE interface so a multi-NIC
    // host streams out a chosen adapter. An unparseable value makes start()
    // fail-closed (visible) rather than silently falling back to all interfaces.
    std::string bindAddress = "0.0.0.0";
    int sendBufferSize = Constants::NETWORK_SEND_BUFFER;  // Official: 65536 (64KB)
    // P0 #19: per-channel async send pool depth. Default keeps the proven
    // libomtnet value (NETWORK_ASYNC_COUNT=4); tunable down (less memory) or up
    // (more in-flight sends) per instance. Clamped to [1,16] in the Channel.
    int sendQueueSize = Constants::NETWORK_ASYNC_COUNT;
    // The per-channel socket-write ceilings (see ChannelBudgets). Defaults are the
    // shipping values; they are configuration rather than constants so the bounded
    // back-pressure paths are observable in a harness in seconds instead of tens of
    // seconds, and so an operator with a genuinely slow WAN viewer can widen them.
    ChannelBudgets budgets;
    // The desktop sender's self-report. "OMT Android Sender" here was inherited from the
    // Android vendor tree that this copy was ported from and never corrected: OBS listed a
    // Linux broadcast node as an Android camera. The Android app keeps its own default in its
    // OWN tree — changing this line does not touch it. Both fields stay overridable through
    // SenderConfig::senderInfoXml or Sender::setSenderInfo(), which is what the node calls.
    // Proven at the wire, not by grepping this header: qa-ompin/wire-slow-reader.cpp case
    // sender-identity connects a real peer and asserts what actually arrived.
    std::string senderInfoXml = "<OMTInfo ProductName=\"nodos OmtOut\" Manufacturer=\"Nodos\" Version=\"1.0\" />";
    int portStart = Constants::NETWORK_PORT_START;
    int portEnd = Constants::NETWORK_PORT_END;
};

/**
 * OMTSender - Main sender class
 * ...
 */
class Sender : public IChannelListener {
public:
    // P0 #9 (Codex): expose the pure frame validator for the native regression
    // harness (same-class access to the private static). No side effects.
    static bool validateFrameForTest(const MediaFrame& frame) { return validateFrame(frame); }

    // P0 #8 (Codex round-5/6): the FULL congestion decision — threshold + quality
    // gate + downgrade map — exposed for the native harness to DIRECTLY observe
    // congestion-adjustment execution (deterministic, not reliant on flaky network
    // backpressure). checkCongestion() calls it UNDER encoderMutex_ so config_.quality
    // and currentProfile_ are read/written under the lock.
    static int congestionDecision(double dropRate, Quality quality, int currentProfile);

    // P0 #8 (Codex round-6): drives the REAL locked congestion state-update (reads
    // config_.quality + writes currentProfile_ + destroys the encoder under
    // encoderMutex_) with an injected drop rate, so the harness can exercise that
    // production path concurrently with setQuality (deterministic, not network-flaky).
    void applyCongestionForTest(double dropRate) { applyCongestionLocked(dropRate); }

    /**
     * Create sender with name and quality
     */
    explicit Sender(const SenderConfig& config = SenderConfig{});
    
    ~Sender();
    
    // Non-copyable
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    
    /**
     * Start accepting connections
     * @return true if started successfully
     */
    bool start();
    // Actual bound port, or zero when stopped. Use this for discovery.
    int port() const { return boundPort_.load(); }
    
    /**
     * Stop sender and disconnect all clients
     */
    void stop();
    
    /**
     * Send a video frame to all subscribed clients
     * @return encoded frame size in bytes, or 0 on failure
     */
    int send(const MediaFrame& frame);
    
    /**
     * Send metadata to all connected clients
     */
    void sendMetadata(const std::string& xml);

    /**
     * Set sender information (product, manufacturer) for handshake.
     */
    void setSenderInfo(const std::string& productName, const std::string& manufacturer);
    
    /**
     * Get current connection count
     */
    int connectionCount() const;

    /**
     * P0 #20 (Codex): count of CONNECTED channels that have actually subscribed
     * to the VIDEO stream — the truthful "Subscribers" figure (connectionCount
     * counts every TCP connection, including audio/metadata-only peers).
     */
    int videoSubscriberCount() const;
    // Test seam: the actual listener descriptor the running sender is bound to,
    // or -1 when not listening. Used by the native teardown cycle test to prove
    // the old fd is closed (EBADF) before any replacement fd is opened.
    int listenerFdForTest() const { return serverSocket_.load(); }
    
    /**
     * Get aggregated statistics
     */
    SenderStats getStats();

    /**
     * Drops since the PREVIOUS call to this method, across every channel (0 when
     * nothing has dropped since you last asked). This is what
     * omt_send_get_recent_drops_and_reset() exposes; before it existed that entry
     * point was a stub returning 0, which made a consumer's congestion alert
     * permanently unreachable (the Android defect, still open on desktop).
     *
     * Reading it CONSUMES the window: each channel's counter is exchanged to 0, and
     * drops carried by channels that were destroyed since the last call are folded in
     * from the sender's own accumulator, so a window is never lost by a teardown and
     * never reported twice.
     */
    int64_t recentDropsAndReset();
    
    /**
     * Get the best suggested quality from all clients
     */
    Quality getBestSuggestedQuality() const;

    /**
     * Get the current VMX profile ID.
     * P0 #8 (Codex): currentProfile_ is written under encoderMutex_ (setQuality /
     * onQualityChanged / checkCongestion), so read it under the same lock.
     */
    int getCurrentProfile() const {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        return currentProfile_;
    }
    
    /**
     * Check if any client has tally
     */
    bool hasTallyPreview() const;
    bool hasTallyProgram() const;
    
    // Callbacks
    using ConnectionCallback = std::function<void(Channel*)>;
    void setOnConnect(ConnectionCallback cb) { onConnect_ = cb; }
    void setOnDisconnect(ConnectionCallback cb) { onDisconnect_ = cb; }
    
    /**
     * Update the quality configuration and reset encoder.
     */
    void setQuality(Quality quality);

private:
    // Configuration
    SenderConfig config_;
    
    // Network
    std::atomic<int> serverSocket_{-1};  // P0 #5: stable across accept-loop vs stop
    std::atomic<int> boundPort_{0};
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    // P0 #5 (Codex): serialize start()/stop() so a concurrent stop can never
    // close/recycle the listener fd while another lifecycle op is in flight, and
    // so stop() can order shutdown -> join -> close deterministically.
    std::mutex lifecycleMutex_;
    // P0 #8 (Codex): guard the sender-info XML that the accept loop reads on the
    // handshake (acceptLoop reads config_.senderInfoXml) against setSenderInfo().
    std::mutex senderInfoMutex_;
    
    // Channels
    std::vector<std::unique_ptr<Channel>> channels_;
    mutable std::mutex channelsMutex_;
    
    // VMX Encoder
    VMX_INSTANCE* vmxInstance_ = nullptr;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    int currentProfile_ = 0;  // VMX_PROFILE stored as int
    int currentColorSpace_ = 0;  // P0 #8: encoder cache key includes colorspace
    mutable std::mutex encoderMutex_;  // P0 #8: mutable so const getters can lock it
    
    // Statistics
    std::atomic<int64_t> totalEncodedBytes_{0};
    std::atomic<int> frameCount_{0};
    /// Un-read recent-drop windows of channels that were destroyed since the last
    /// recentDropsAndReset() (see the banking in removeChannel/cleanupDisconnected).
    std::atomic<int64_t> retiredRecentDrops_{0};
    
    // Callbacks
    ConnectionCallback onConnect_;
    ConnectionCallback onDisconnect_;
    
    // Accept loop
    void acceptLoop();
    
    // Add/remove channels
    void addChannel(std::unique_ptr<Channel> channel);
    void removeChannel(Channel* channel);
    int cleanupDisconnected();
    
    // Encoder management
    void createEncoder(int width, int height, int frameRate, int colorSpace);
    // P0 #8 (Codex): encoder-select + encode + payload-copy as ONE transaction
    // under a single encoderMutex_ hold (caller must already hold the lock).
    // P0 #9 (Codex): reject out-of-range dimensions/stride/length before alloc/VMX.
    static bool validateFrame(const MediaFrame& frame);
    bool ensureEncoderLocked(int width, int height, int colorSpace);
    int encodeFrameLocked(const MediaFrame& frame, std::vector<uint8_t>& out);
    void destroyEncoder();
    int encodeFrame(const MediaFrame& frame);
    
    // Buffer size calculation
    static int calculateOptimalBuffer(int width, int height, int profile);

    // Congestion Control
    void checkCongestion();
    void applyCongestionLocked(double dropRate);   // P0 #8 (Codex): the locked state-update, shared with applyCongestionForTest
    
    // IChannelListener implementation
    void onDisconnected(Channel* channel) override;
    void onTallyChanged(Channel* channel) override;
    void onQualityChanged(Channel* channel) override;
    
    // Profile selection
    int selectProfile(Quality quality) const;
};

} // namespace omt

#endif // OMT_SENDER_H
