## 2026-10-25 — Avoid video/audio encoding overhead when no clients are subscribed

**Learning:** `OMTSender::send` and `OMTSender::sendAudio` were previously performing full frame validations, audio plane zero checks, and VMX video encodings even when connected clients had not subscribed to video or audio streams. Consolidating disconnected channel cleanup and subscriber counting into `cleanupAndCountSubscribers()` under a single `channelsMutex_` lock pass allowed early-exit checks when `videoSubscribers == 0` or `audioSubscribers == 0`, eliminating redundant CPU and memory bandwidth overhead.

**Action:** Before performing heavy encoding or transformation work in network transport sender paths, check active subscription counts during channel cleanup under a single lock pass.
