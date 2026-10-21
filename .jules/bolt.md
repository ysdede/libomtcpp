## 2026-03-06 — Amortizing Video Buffer Resizing in OMT Receiver

**Learning:** `std::vector::assign(bytes, 0)` on every video frame in `Receiver::Impl::decode` forces `memset` zero-initialization of 8MB–33MB per frame (0.41ms at 1080p, 4.80ms at 4K). Because VMX decoders immediately overwrite the full frame buffer, this zeroing is 100% redundant and consumes up to 29% of the 60fps frame budget at 4K.

**Action:** Prefer `if (vec.size() != bytes) vec.resize(bytes);` in steady-state decoding loops where decoders fill the full buffer payload, avoiding per-frame memory clearing.
