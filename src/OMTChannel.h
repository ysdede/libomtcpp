/**
 * OMTChannel.h - Client Connection Channel
 * Port of libomtnet/src/OMTChannel.cs
 * 
 * Represents a single client connection with:
 * - Async send via buffer pool
 * - Receiver thread for metadata processing
 * - Subscription, tally, and quality state
 */

#ifndef OMT_CHANNEL_H
#define OMT_CHANNEL_H

#include <atomic>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#ifdef _WIN32
  #define OMT_OS_WIN_TYPES 1
  #include "omt_os_win.h"
  #undef OMT_OS_WIN_TYPES
#else
  #include <arpa/inet.h>
#endif

#include "OMTConstants.h"
#include "OMTProtocol.h"
#include "OMTAsyncPool.h"

namespace omt {

// Forward declarations
class Sender;

/**
 * Why a BOUNDED socket write stopped before the frame was complete.
 *
 * The distinction is the entire point of this type. A peer that merely stopped
 * draining (Timeout) is alive: OBS pauses its decoder, its receive window closes
 * for a moment, and the bytes we already committed are still going to be read. A
 * peer that is gone (PeerGone) is not: the kernel told us so (EPIPE/ECONNRESET/
 * ENOTCONN). The desktop sender used to treat both as death, and the measured
 * result on the live engine was 802 channel teardowns in three minutes against a
 * subscribed OBS — ~4.5 kills per second, with video only visible because the
 * viewer reconnected that fast. qa-ompin/wire-slow-reader.cpp is the harness that
 * reproduces it over a real socket and pins the difference down.
 */
enum class SendOutcome : uint8_t {
    Ok       = 0,   // every byte reached the socket
    Timeout  = 1,   // the budget expired: ALIVE, just not draining — back-pressure
    PeerGone = 2    // a hard socket error: the connection is dead
};

/**
 * Ceilings for one channel's socket-write pipeline. Every field is a ceiling,
 * never a "block until it finishes": the whole class of bug this file carries is
 * bounded waits that were mistaken for dead peers. The defaults are the shipping
 * values; a harness may lower them to make the bounded paths observable in seconds
 * instead of tens of seconds (qa-ompin/wire-slow-reader.cpp does).
 */
struct ChannelBudgets {
    int lockBudgetMs  = 6000;  // sendMetadataSync's wait for the shared write lock
    int sendTimeoutMs = 5000;  // how long ONE frame may take to reach the socket
    int flushBudgetMs = 200;   // how long a half-written frame gets on an idle tick
    int stalledPeerMs = 30000; // no byte of progress for this long -> the peer is gone
};

/**
 * Channel statistics
 */
// Frame accounting is a PARTITION, not a set of independent tallies: every frame
// handed to sendAsync increments framesOffered exactly once, and exactly one of the
// three terminal counters when the frame's life ends:
//   framesSent    - the whole record reached the socket;
//   framesDropped - it never began a write (ring full, oversized, channel already
//                   gone, or still queued when the channel was torn down);
//   framesTorn    - a write BEGAN and failed partway (peer died mid-frame / the
//                   sendAllPoll deadline expired). Half the record is on the wire and
//                   the channel is dead.
// Therefore, at quiescence (nothing left in the queue and no write in flight):
//   framesOffered == framesSent + framesDropped + framesTorn
// At any instant the stronger form holds with pendingDepth() added — see the
// `torn-write` case in qa-omp/review-native.cpp, which checks both.
struct ChannelStats {
    int64_t framesSent = 0;
    int64_t framesDropped = 0;
    int64_t bytesSent = 0;
    int64_t bytesReceived = 0;
    int64_t framesOffered = 0;
    int64_t framesTorn = 0;
};

/**
 * Callback interface for channel events
 */
class IChannelListener {
public:
    virtual ~IChannelListener() = default;
    virtual void onDisconnected(class Channel* channel) = 0;
    virtual void onTallyChanged(class Channel* channel) = 0;
    virtual void onQualityChanged(class Channel* channel) = 0;
};

/**
 * OMTChannel - Single client connection
 * 
 * Design Pattern: Active Object
 * - Sender thread handles async sends from queue
 * - Receiver thread handles incoming metadata
 * - Thread-safe state management via atomics
 */
class Channel {
public:
    Channel(int socketFd, struct sockaddr_in address, 
            int sendBufferSize = Constants::NETWORK_SEND_BUFFER,  // Official: 65536 (64KB)
            IChannelListener* listener = nullptr,
            int sendQueueCount = Constants::NETWORK_ASYNC_COUNT,  // P0 #19: configurable async send pool depth
            const ChannelBudgets& budgets = ChannelBudgets{});    // see ChannelBudgets
    
    ~Channel();
    
    // Non-copyable
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    
    // Connection state
    bool isConnected() const { return connected_.load(); }
    void disconnect();
    
    // Subscriptions
    Subscription getSubscriptions() const { return subscriptions_.load(); }
    bool isVideoSubscribed() const { return hasFlag(subscriptions_.load(), Subscription::Video); }
    bool isAudioSubscribed() const { return hasFlag(subscriptions_.load(), Subscription::Audio); }
    bool isMetadataSubscribed() const { return hasFlag(subscriptions_.load(), Subscription::Metadata); }
    
    // Quality negotiation
    Quality getSuggestedQuality() const { return suggestedQuality_.load(); }
    
    // Tally state
    bool isTallyPreview() const { return tallyPreview_.load(); }
    bool isTallyProgram() const { return tallyProgram_.load(); }
    
    // Preview mode
    bool isPreviewMode() const { return previewMode_.load(); }
    
    // Send methods
    int sendAsync(const void* headerData, size_t headerLen,
                  const void* extHeaderData, size_t extHeaderLen,
                  const void* payloadData, size_t payloadLen);
    
    bool sendMetadataSync(const char* xml);

    /**
     * Queue a metadata (control) frame for the channel's own writer thread, to be
     * written as soon as the socket will take it. Non-blocking, and the ONE thing
     * that makes the accept-time OMTInfo banner survivable: a peer that is not
     * draining at the moment we accept it must still receive the banner the second
     * it starts reading, and the accept loop must not sit in a 5-second write for
     * the sake of a viewer nobody has accepted yet.
     *
     * Bounded by construction: there is exactly ONE waiting slot, and a newer
     * metadata frame replaces the unsent older one (metadata is state, not a
     * stream — the last OMTInfo is the true one). So the queue can never grow, and
     * metadataSuperseded() counts what a replacement cost.
     */
    bool queueMetadata(const char* xml);
    void queueTally(bool preview, bool program);

    // Observability for the harnesses (qa-ompin/wire-slow-reader.cpp). Each counter
    // is the only way to see a property that is invisible in the received bytes:
    //   metadataAttempts()   proves the banner was genuinely RETRIED, not merely
    //                        written on the first try (an anti-vacuity pin).
    //   metadataSuperseded() proves the single waiting slot stayed single while
    //                        dozens of frames were queued behind it.
    int64_t metadataAttempts() const { return metadataAttempts_.load(); }
    int64_t metadataSuperseded() const { return metadataSuperseded_.load(); }
    /// Bytes currently held in the ONE waiting metadata slot (<= one control frame).
    int64_t queuedMetadataBytes() const;
    /// True while a half-written frame owns the byte stream.
    bool hasPendingWriteForTest() const;
    
    // Statistics
    ChannelStats getStats() const;

    /// Queued frames the sender threw away to make room for a NEWER one (see the
    /// drop-oldest policy in sendAsync). A subset of framesDropped_: a frame that found
    /// the ring full with nothing left to evict is counted in framesDropped_ only.
    int64_t staleEvicted() const { return staleEvicted_.load(); }

    /// Frames still waiting in pendingQueue_ — neither sent nor dropped yet. The
    /// reconciliation identity is offered == sent + dropped + torn + pendingDepth().
    int64_t pendingDepth() const;

    /// Throw away everything still queued, counting each frame as dropped. ~Channel
    /// calls this once both threads have joined (so a frame cannot be counted twice);
    /// it is public because the reconciliation must be observable BEFORE teardown —
    /// after ~Channel the counters are gone with the channel.
    int64_t discardPending();

    /// Drops since the previous call to this method (0 when nobody reported them).
    /// This is the counter omt_send_get_recent_drops_and_reset() exposes: a UI that
    /// polls it shows "drops in the last poll interval", not a lifetime total it has
    /// to difference itself. Consumes the window (exchange, not read), so a caller
    /// that reads it twice in a row sees the drops once and then 0.
    ///
    /// A channel that dies with an un-read window does NOT lose those drops:
    /// Sender::removeChannel() folds takeRecentDrops() into its own accumulator before
    /// destroying the channel, so the window survives the teardown.
    int64_t takeRecentDrops() { return recentDrops_.exchange(0); }

    // Endpoint info
    std::string getRemoteAddress() const;
    int getRemotePort() const;

private:
    // Socket
    std::atomic<int> socketFd_{-1};
    struct sockaddr_in address_;
    std::atomic<bool> connected_{true};
    std::atomic<bool> running_{true};
    
    // Async send infrastructure
    std::unique_ptr<AsyncPool> sendPool_;
    int sendQueueCount_ = Constants::NETWORK_ASYNC_COUNT; // P0 #19
    std::queue<AsyncBuffer*> pendingQueue_;
    mutable std::mutex queueMutex_;   // const-pendingDepth() reads the queue under it
    std::condition_variable queueCV_;
    std::thread senderThread_;
    std::thread receiverThread_;

    ChannelBudgets budgets_{};                       // the ceilings, set once at construction

    // ------------------------------------------------------------------ the socket-write pipeline
    //
    // ONE half-written frame, owned by whoever holds sendSocketMutex_. This exists
    // because "do not tear the channel down on a slow peer" is only safe if the
    // bytes already committed to the stream are finished before ANY other byte is
    // written: a metadata frame cut in half and abandoned desynchronizes the peer's
    // framer for the rest of the connection, which is exactly why the original code
    // reached for connected_=false. Keeping the remainder here turns the same event
    // into back-pressure — the next writer completes it first, then carries on.
    // Touched ONLY while sendSocketMutex_ is held (never under queueMutex_).
    std::vector<uint8_t> continuation_;
    size_t continuationOffset_ = 0;
    bool continuationActive_ = false;
    bool continuationIsVideo_ = false;
    // Wake-up hint for senderLoop, so it keeps nursing the half-written frame even
    // on an otherwise idle tick. Written by any writer, read by the CV predicate;
    // the authoritative state is continuationActive_ above, under the write lock.
    std::atomic<bool> continuationPending_{false};

    // The ONE metadata frame waiting for the socket (see queueMetadata). Under
    // queueMutex_, single-slot, newest wins — it cannot grow.
    std::vector<uint8_t> pendingMetadata_;
    bool pendingMetadataActive_ = false;
    // Independent coalesced control slot: tally cannot replace the greeting.
    std::vector<uint8_t> pendingTally_;
    void holdTallyForRetry(std::vector<uint8_t> frame, bool onlyIfEmpty);

    // Liveness, as opposed to back-pressure: the last moment any byte of ours
    // actually reached the socket. If a channel has work to send and has made no
    // progress at all for budgets_.stalledPeerMs, the peer is not merely slow any
    // more and the channel is reaped — otherwise "slow is not dead" would quietly
    // mean "a viewer that freezes never leaves", and TCP keepalive (5s idle, but
    // 75s x 9 probes before it errors anything) is far too slow to be the bound.
    std::atomic<int64_t> progressUs_{0};

    // The reader thread's verdict, NOT the observable state. connected_ is what the
    // node, the sender and the QA harnesses read, and the writer is the only thread
    // allowed to flip it: it owns the in-flight frame and the terminal counters, and
    // a death published by anybody else could be observed while that frame was still
    // in none of sent/dropped/torn. The reader sets THIS instead (markPeerGone), the
    // writer stops its bounded write on it and then publishes connected_ itself.
    std::atomic<bool> peerGone_{false};
    // True once senderLoop has filed every frame it owned and left. Lets markPeerGone
    // know there is no writer left to hand the reap to.
    std::atomic<bool> senderLoopDone_{false};

    // Log-once-per-storm, per kind: 0 = video write, 1 = metadata write,
    // 2 = socket-write-lock budget, 3 = stalled-peer reap. The live failure was
    // 802 identical warning lines in three minutes; a warning that repeats once
    // per retry is not a diagnostic, it is the outage. Each flag clears again when
    // a write fully succeeds, so a NEW stall is still reported.
    std::atomic<bool> warnedWrite_[4] = { {false}, {false}, {false}, {false} };

    std::atomic<int64_t> metadataAttempts_{0};
    std::atomic<int64_t> metadataSuperseded_{0};

    
    // P0 #7 (Codex): serialize ALL socket writes (async video from senderLoop
    // and synchronous metadata from sendMetadataSync) so partial writes of the
    // two paths can never interleave on the single TCP byte stream.
    // Plain std::mutex (NOT timed_mutex): the sync metadata writer bounds its
    // acquisition with a try_lock() loop (std::mutex::try_lock is the path TSan
    // models correctly; std::timed_mutex's timed acquisition produced spurious
    // "unlock of an unlocked mutex" reports under TSan on this libstdc++).
    // mutable: hasPendingWriteForTest() peeks under it from a const method.
    mutable std::mutex sendSocketMutex_;
    // P0 #6 (Codex): read-half EOF is a state, not a 20ms poll loop. Once the
    // peer closes its write half we stop reading but keep sending.
    bool readEof_ = false;
    
    // Subscriptions and state
    std::atomic<Subscription> subscriptions_{Subscription::None};
    std::atomic<Quality> suggestedQuality_{Quality::Default};
    std::atomic<bool> tallyPreview_{false};
    std::atomic<bool> tallyProgram_{false};
    std::atomic<bool> previewMode_{false};
    
    // Statistics
    std::atomic<int64_t> framesSent_{0};
    std::atomic<int64_t> framesDropped_{0};
    /// Every frame handed to sendAsync, counted at the door before any early return.
    /// The reconciliation invariant (see ChannelStats) is
    /// framesOffered == framesSent + framesDropped + framesTorn + pendingQueue depth,
    /// so a frame can neither vanish from the statistics nor be counted twice.
    std::atomic<int64_t> framesOffered_{0};
    /// Dequeued frames whose write began and failed partway (peer died / deadline).
    /// Disjoint from framesDropped_: a torn frame got on the wire, a dropped one never did.
    std::atomic<int64_t> framesTorn_{0};
    /// Frames evicted from the queue by the drop-oldest policy (see sendAsync).
    std::atomic<int64_t> staleEvicted_{0};
    /// Drops since someone last asked (see takeRecentDrops) — the window the
    /// congestion alert in a UI reads. Incremented ONLY inside countDropped(), so it
    /// cannot drift from framesDropped_ the way a second tally at each call site would.
    std::atomic<int64_t> recentDrops_{0};
    std::atomic<int64_t> bytesSent_{0};
    std::atomic<int64_t> bytesReceived_{0};

    /// The ONE place a frame is counted as dropped, so no early-return path in
    /// sendAsync can forget the tally and the drop counters cannot drift apart.
    /// Both the lifetime counter and the recent-drop window move HERE, never at a
    /// call site — a drop that is invisible to recentDrops_ is a congestion alert
    /// that never fires (the exact Android defect this port closes on desktop).
    void countDropped() { framesDropped_++; recentDrops_++; }

    /// The ONE place a torn write is counted (see countDropped for the same
    /// single-site discipline): a frame whose write BEGAN and will never complete
    /// (peer died under it, the bounded write expired, or the loop is ending
    /// around a retained continuation). Only video frames join the
    /// offered/sent/dropped/torn partition, so both guards are explicit at the
    /// call sites — senderLoop's PeerGone, the death of a retained continuation,
    /// and the loop-exit sweep. Disjoint from countDropped by construction.
    void countTorn(bool beganWrite, bool isVideo);
    
    // Listener
    IChannelListener* listener_;
    
    // Thread functions
    void senderLoop();
    void receiverLoop();
    
    // Internal send: the async video writer and the sync metadata writer share ONE
    // critical section (sendSocketMutex_) and one continuation slot, so a partial
    // write of either is always finished before any other byte goes on the stream.
    // (The old `sendAllBlocking` helper is gone: it called the same poll loop and
    // then called the channel dead for any reason at all, which was the bug.)

    // ------------------------------------------------------------------ socket-write pipeline helpers
    // Every one of these is called ONLY while sendSocketMutex_ is held.
    SendOutcome flushContinuationLocked(int fd, int timeoutMs);
    SendOutcome startFrameLocked(int fd, const uint8_t* data, size_t length,
                                 bool isVideo, int timeoutMs);
    void clearContinuationLocked();
    // File the frame the write pipeline still owns (torn, once) and release it.
    // sendSocketMutex_ held.
    void settleContinuationLocked();
    // End the channel from inside sendSocketMutex_: settle, then publish the death,
    // in that order and indivisibly — no observer may read a disconnected channel
    // whose last frame is in none of the counters.
    void publishPeerGoneLocked();
    // The reader thread's peer-is-gone verdict. It signals peerGone_ and lets the
    // writer publish connected_; it only reaps directly if the writer has already
    // exited. NEVER call it from the writer thread (it waits for that thread).
    void markPeerGone();
    // Install `frame` into the single waiting-metadata slot (newest wins). Takes
    // queueMutex_ itself, so callers must hold NO lock.
    void holdMetadataForRetry(std::vector<uint8_t> frame);
    // One warning per stall episode per `kind` (see warnedWrite_), not per attempt.
    void warnOnce(int kind, const char* fmt, ...);
    void noteProgress() {
        progressUs_.store(SteadyUs(), std::memory_order_relaxed);
    }
    static int64_t SteadyUs();
    
    // Metadata processing (port of ProcessMetadata)
    void processMetadata(const std::string& xml);
    
    // Socket helpers
    void setSendBuffer(int size);
    void closeSocket();
};

} // namespace omt

#endif // OMT_CHANNEL_H
