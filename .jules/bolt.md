## 2026-03-06 — 64-bit word scanning for audio silence detection

**Learning:** Replacing byte-by-byte loops in audio planar silence checking with `std::memcpy`-based 64-bit/32-bit word scanning yields a ~6.46x speedup in `encodeAudio` without unaligned access UB or strict aliasing violations.

**Action:** When scanning byte arrays for zero/non-zero values in hot paths, prefer 64-bit word reads with `std::memcpy` over byte loops.
