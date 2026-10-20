# Tally, preview, audio and receiver port

Validated 2026-10-20. The reference is unmodified libomtnet
`7d29031ff3f355ffa1f0cd08ee24d02e258445dd`. The codec is ysdede/libvmx
`89b0986c1a2d8b81e4e5230a346b6285cb9367be` plus PR #8, which fixes
undefined signed DC reconstruction and rejects invalid shifts/lengths.

## Verified behavior

- Windows x64 Release: six CTest executables pass. Linux x64 Release with
  ASan/UBSan and `-fno-sanitize-recover=all`: the same six pass.
- Real codec tests distinguish full and preview payloads, compare decoded pixels
  with a separate VMX decoder, and switch preview off on an existing connection.
- Receiver tests cover preferred formats with/without alpha, 1080 interlaced
  full/preview, PA16 high-bit alpha, compressed-only data and ownership across
  audio/video calls. They reject DC-only data presented as full video, so a
  malformed frame cannot reuse the previous AC planes.
- Independent little-endian FPA1 bytes test the 24-byte header, original float
  bits, silent planes, channel 32, attached metadata and malformed masks/lengths.
- TCP tests cover fragmented records, oversized headers, reconnect, bounded
  oldest-frame eviction, application metadata timestamps, tally clear events and
  destruction during socket activity. Connection changes clear queued media.
- Unmodified official C# sender/receiver: **40 checks pass in both directions**,
  using full/preview video, FPA1, combined tally and application metadata.
- Nodos sender native regression suite: **19/19 cases**, no ASan/UBSan output.
  This includes retained TCP continuations, half-close, slow consumers, concurrent
  producers, teardown and the frame-accounting invariant.

These are bounded correctness and interoperability tests, not a new 4K60
performance claim, long endurance certification, ARM execution test or IPv6
network validation. The new C receiver does not replace the Nodos-specific
receiver automatically. Nodos audio pins/playout are not part of this port.

## Important compatibility findings

### Standalone metadata is not attached frame metadata

The C API uses NUL-terminated strings. The reference `OMTBuffer.FromMetadata`
and `OMTChannel.Send(OMTMetadata)` send standalone UTF-8 XML without that NUL,
with `metadataLength=0`. Attached audio/video metadata instead uses the header
metadata length and includes the NUL. The reference matches control XML strings
exactly, so appending a wire NUL to subscribe/preview/tally prevents recognition.

The C++ sender and receiver now emit the reference standalone form and accept
both forms. An incoming standalone metadataLength may be zero or the full
payload size, but cannot be an inconsistent partial size. Receiver output adds
its own NUL for the C ABI. Preview/quality controls go only to the video socket;
applying preview to the official audio socket can produce invalid audio framing.

### Known defect in the reference audio sender

With attached audio metadata, the pinned reference's `OMTSend.SendAudio` calls
`OMTFPA1Codec.Encode`, which leaves the destination offset at zero, then calls
`OMTBuffer.Append`. Append writes metadata at that offset, overwriting the first
encoded audio samples. The negative interoperability run observed the bytes of
`<fixture` where the samples `+1, -1` should be.

The final official-to-C++ audio fixture therefore uses no attached metadata.
Attached metadata is tested independently with golden bytes and C++-to-official
transport; those pass. This upstream defect is recorded, not reproduced in the
C++ implementation. General application metadata passes both directions.

### VMX undefined behavior

Preview testing under C++17 UBSan found a left shift of negative DC coefficients.
PR #8 replaces the eight reconstruction shifts with bounded multiplication and
validates the shift parameter. Old and fixed codec full/preview pixel hash:
`18e840f2377e8c59`, for shifts 0–3 on the test raster. ARM expressions were fixed
consistently but were not executed. The receiver additionally verifies complete
DC/AC length tables before invoking the codec.

## Reproduce

Use a libvmx checkout containing PR #8. Control tests deliberately use a failing
codec stub; video tests must explicitly enable the real codec.

```sh
cmake -S . -B build/tests -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DLIBOMTCPP_BUILD_TESTS=ON -DLIBOMTCPP_TEST_REAL_VMX=ON -DVMX_ROOT=/path/to/libvmx
cmake --build build/tests --config Release --parallel 4
ctest --test-dir build/tests -C Release --output-on-failure
```

For Linux sanitizers add
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"`.

The optional Windows interoperability project compiles the reference sources
in place, without modifying them. Use a checkout at the reference SHA above and
Visual Studio's Roslyn `csc.exe`:

```powershell
cmake -S tests/interop -B build/interop `
  -DVMX_ROOT=C:/src/libvmx -DLIBOMTNET_ROOT=C:/src/libomtnet `
  "-DCSC_EXECUTABLE=C:/path/to/MSBuild/Current/Bin/Roslyn/csc.exe"
cmake --build build/interop --config Release --parallel 4
ctest --test-dir build/interop -C Release --output-on-failure
```

Fixtures use loopback ports 26490–26499 and an isolated settings directory. The
reference discovery server is directed to an unused loopback endpoint so the
fixture does not advertise test streams on the production LAN.

## Remaining boundaries

The full [protocol/API matrix](PROTOCOL-PARITY.md) remains authoritative.
Not yet ported: native DNS-SD/discovery server, redirect following, live DNS
refresh, retained connection metadata, sender video attached metadata, compressed
send passthrough, extra raw sender formats and Timestamp=-1 pacing. Sender
statistics currently aggregate active channels and initialize unsupported fields
to zero; lifetime/interval statistics parity is not claimed.
