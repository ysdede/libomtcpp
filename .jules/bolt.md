## 2026-10-27 — Fast 64-bit Word Scanning for FPA1 Audio Plane Silence Detection

**Learning:** `encodeAudio` in `OMTAudio.h` checked for non-silent audio planes using a byte-by-byte loop (`for (size_t i = 0; i < plane; ++i) if (p[i])`). Scanning 64-bit words via `std::memcpy` (to remain alignment-safe) and replacing `std::vector::insert` with `output.resize()` + `std::memcpy()` reduced encoding overhead by ~5x for multi-channel audio frames.

**Action:** When scanning uncompressed sample or pixel buffers for zero/transparency or copying contiguous blocks, prefer 64-bit word checking and explicit `std::memcpy` over byte-level iteration and container `insert` range operations.
