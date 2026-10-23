## 2026-10-23 — Audio Plane Zero-Checking and Buffer Append Optimization

**Learning:** Replacing scalar byte-by-byte zero checking on audio planes in `encodeAudio` with 64-bit word (`uint64_t`) scanning via `std::memcpy` and replacing iterator `std::vector::insert` with direct `std::memcpy` yielded a 5.7x–7.7x speedup (reducing 100,000 iterations from 5089ms to 655ms for silent planes, and 5248ms to 913ms for active planes).

**Action:** Use 64-bit word scanning and batch `memcpy` operations on planar memory checks/copies across hot streaming paths.
