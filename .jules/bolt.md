## 2026-10-26 — Audio Plane Silence Detection Chunking

**Learning:** Byte-by-byte scanning in `encodeAudio` to detect active audio channels imposed a 10x CPU penalty on multi-channel silent/partially-active audio streams (e.g. 16 or 32 channels). Replacing single-byte iteration with 64-bit uint64_t chunk scanning via `std::memcpy` improved `encodeAudio` throughput by 7.5x-10.2x without altering bitwise layout or wire behavior.

**Action:** When scanning multi-channel float/pcm buffers for silence/activity where buffer size is aligned to 4/8 bytes, always use uint64_t chunk scanning to allow vectorization and reduce loop iterations.
