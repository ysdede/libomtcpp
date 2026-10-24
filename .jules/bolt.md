## 2026-10-24 — FPA1 Audio Plane Zero Scanning and Vector Insert Overhead

**Learning:** Scalar byte-by-byte zero checking on audio sample planes in `encodeAudio` accounted for significant CPU overhead (~1.67s for 200k 16-channel frames). Scanning 64-bit words via `std::memcpy` in `isPlaneActive()` combined with pre-resizing output vectors and direct `std::memcpy` yielded a 3.18x speedup (~539ms) in audio encoding.

**Action:** Prefer 64-bit word scanning (`uint64_t` via `std::memcpy`) for zero-checks on byte buffers, and avoid element-by-element `std::vector::insert` loops in hot path frame coders.
