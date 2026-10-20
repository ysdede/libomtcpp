# libomtcpp

A native C++17 implementation of the [Open Media Transport (OMT)](https://github.com/openmediatransport) protocol for low-latency video streaming.

Provides TCP transport, VMX video, FPA1 audio, multi-client sending and a bounded receiver C API.

This implements a subset of libomtnet. Discovery backends, redirects and some
convenience APIs remain incomplete. See the
[protocol/API support matrix](docs/PROTOCOL-PARITY.md) for supported behavior,
remaining gaps and validation.

## Features

- VMX video and reduced preview transport
- FPA1 planar audio with silent-channel elision
- Reconnecting receiver with bounded queues and compressed video access
- Multi-client streaming with per-channel threading
- Quality negotiation (LQ/SQ/HQ)
- Combined preview/program tally, including late joiners
- Sender identification and receiver-to-sender application metadata
- Reusable buffer pools
- C API for integration with native applications
- Android NDK support

## Used by

[Nodos OMT](https://github.com/yd-zd/nodos-omt) uses libomtcpp for native OMT video output. Its integration tests exercise the sender, channel lifecycle, and wire protocol on Windows and Linux.

## Requirements

- C++17 compiler
- CMake 3.14+
- [libvmx](https://github.com/openmediatransport/libvmx) - VMX video codec

## Building

```bash
# Clone libvmx alongside libomtcpp
git clone https://github.com/openmediatransport/libvmx.git ../libvmx

# Configure and build
cmake -S . -B build -DVMX_ROOT=../libvmx
cmake --build build --config Release
```

### Android (NDK)

```bash
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DVMX_ROOT=/path/to/libvmx

cmake --build build-android
```

## Usage

Senders allocate a free TCP port in 6400â€“6600 by default, matching libomtnet.
In C++, `SenderConfig.port = 0` enables automatic allocation; `portStart` and
`portEnd` customize its range. A nonzero `port` is exact and fails on conflict.
Use `Sender::port()` or `omt_send_getport()` after successful startup when
registering discovery or displaying a direct address. Each independent stream
needs its own listening endpoint; different names do not multiplex one port.
The chosen port remains stable until the sender stops.

```cpp
#include "libomt.h"

// Create sender
omt_send_t* sender = omt_send_create("MyCamera", OMTQuality_Medium);

// Prepare video frame
OMTMediaFrame frame = {};
frame.Type = OMTFrameType_Video;
frame.Codec = OMTCodec_NV12;
frame.Width = 1920;
frame.Height = 1080;
frame.Data = yuv_data;
frame.DataLength = data_size;
frame.FrameRateN = 30;
frame.FrameRateD = 1;

// Send frame
omt_send(sender, &frame);

// Release sender
omt_send_destroy(sender);
```

Supported sender inputs are BGRA, BGRX, UYVY, YUY2 and NV12.

## mDNS Discovery

Service discovery is delegated to the host application. The library exposes discovery callbacks but does not include an internal mDNS implementation.

| Platform | Discovery backend |
|---|---|
| Android | NsdManager (JNI) |
| Linux | Host-provided backend, for example Avahi |
| macOS / Windows | Host-provided DNS-SD backend |

Discovery callbacks:

- `omt_discovery_on_service_found()`
- `omt_discovery_on_service_lost()`
- `omt_discovery_on_registered()`

## Engineering Principles

1. **Allocation-Efficient Hot Paths** - Minimize steady-state heap allocations in send, receive, and media-processing loops. Prefer reusable buffers and predictable memory usage.

2. **RAII & Memory Safety** - Enforce deterministic ownership of sockets, threads, and media buffers. Keep C API boundaries exception-safe.

3. **Concurrency & Low Latency** - Minimize contention between network I/O and media processing. Prefer bounded queues and measured synchronization strategies over unnecessary lock-free complexity.

4. **Protocol & Platform Compatibility** - Preserve OMT wire compatibility, public API/ABI contracts, and supported platform behavior.

5. **Measured Optimization** - Prioritize reproducible improvements in throughput, frame latency, memory efficiency, and CPU utilization without compromising correctness.

## License

MIT License. See [LICENSE.txt](LICENSE.txt).

## Receiver ownership and limits

`omt_receive_create("omt://host:port", types, format, flags)` starts independent
video/metadata and audio connections. Direct IPv4/IPv6 endpoints and names already
resolved by the host discovery adapter are accepted. DNS is resolved during
creation; redirect following and live address refresh are not implemented.

Each receive result owns its raster, compressed bytes and attached metadata until
the next receive call for that **same media type**, or receiver destruction.
Do not call receive concurrently for the same type, and quiesce API callers before
destroying an instance. Audio and video may be consumed separately. Queue limits
are 4 video, 10 audio and 60 metadata frames; overflow drops the oldest whole
queued frame. Reconnect clears the affected queue and does not replay pending
application metadata from the old connection.

VMX receive supports BGRA, UYVY, UYVA, P216 and PA16 according to the preferred
format and wire alpha/high-bit-depth flags. Preview with a P216 preference returns
UYVY because VMX preview is 8-bit. Compressed-only receive avoids raster decoding.
Malformed sizes, channel masks and incomplete full VMX payloads are rejected.

Audio is planar float32 FPA1, 1–32 channels, at most 1 MiB of decoded samples per
frame. Standalone XML is sent without a wire NUL, matching libomtnet; returned
C API XML includes a NUL. Both terminated and unterminated incoming XML are
accepted. Application `omt_send_receive` uses a bounded 60-message queue.

See [media-port validation](docs/MEDIA-PORT-VALIDATION.md) for reproducible tests,
known upstream defects and the supported matrix. This does not add audio pins to
applications that currently use only the video sender.
