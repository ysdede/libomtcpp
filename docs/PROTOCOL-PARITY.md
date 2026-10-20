# OMT protocol and API parity

Reviewed 2026-10-20 against libomtnet
[`7d29031`](https://github.com/openmediatransport/libomtnet/tree/7d29031ff3f355ffa1f0cd08ee24d02e258445dd)
and libomtcpp `767a68f250c199650aae87abbcf4c64ddadb466f`.
This is a source-level capability inventory, with separately identified tests.
It is not a full interoperability certification.

The earlier review of commits since January 2026 was insufficient: automatic
sender port allocation already existed before that interval. Future reviews
must compare the complete protocol/API surface as well as new commits.

## Scope and authorities

- [Protocol 1.0](https://github.com/openmediatransport/libomtnet/blob/7d29031ff3f355ffa1f0cd08ee24d02e258445dd/PROTOCOL.md):
  framing, metadata commands, DNS-SD and discovery server.
- [Public types](https://github.com/openmediatransport/libomtnet/blob/7d29031ff3f355ffa1f0cd08ee24d02e258445dd/src/OMTPublicTypes.cs):
  formats, flags, timestamps, quality and API data.
- [Sender](https://github.com/openmediatransport/libomtnet/blob/7d29031ff3f355ffa1f0cd08ee24d02e258445dd/src/OMTSend.cs),
  [channel](https://github.com/openmediatransport/libomtnet/blob/7d29031ff3f355ffa1f0cd08ee24d02e258445dd/src/OMTChannel.cs)
  and [receiver](https://github.com/openmediatransport/libomtnet/blob/7d29031ff3f355ffa1f0cd08ee24d02e258445dd/src/OMTReceive.cs)
  define implementation behavior beyond the wire overview.
- Additional surfaces inspected: OMTFrame, OMTClock, OMTAddress, OMTMetadata,
  OMTSendReceiveBase, OMTSettings, OMTDiscovery, discovery client/server,
  OMTMultiConnectSocket and OMTRedirect.

The C++ library currently implements an IPv4 VMX video sender and incoming
control messages. Its public C header contains additional declarations whose
implementations are stubs. Nodos supplies its own receiver and DNS backends;
their existence does not establish library parity.

## Protocol matrix

| Surface | C++ status and evidence | Remaining acceptance check |
| --- | --- | --- |
| Frame header | 16-byte header, version 1, 100 ns timestamp, video type 2; `OMTProtocol.h`, `Sender::send` | Golden little-endian bytes and malformed-record cases; direct struct copies assume little-endian hosts |
| Video extended header | 32 bytes; dimensions, rational rate, aspect, flags, matrix emitted | Independent receiver for every supported format/flag combination |
| VMX video | BGRA, BGRX, UYVY, YUY2, NV12 accepted by `validateFrame`/`encodeFrameLocked` | Real-codec suite exists; do not infer support from header enum declarations |
| Other raw inputs | YV12, UYVA, P216, PA16 declared in C header but rejected by sender | Explicit implementations, strides, alpha and high-bit-depth pixel oracles |
| Compressed VMX passthrough | Missing: VMX1 input rejected as an unsupported raw codec | Preserve compressed bytes and per-frame metadata without re-encoding |
| Alpha semantics | Numeric flags match; encode dispatch follows BGRA/BGRX codec rather than the official BGRA + Alpha flag rule | BGRA without Alpha must behave as opaque; reject contradictory Preview/HighBitDepth flags for ordinary raw inputs |
| Interlace | Explicitly rejects unsupported interlaced heights rather than advertising progressive payload as interlaced | Cross-implementation field order/cadence at 480/576/1080 |
| Color matrix | 0/601/709 accepted, matching official public enum; no BT.2020 matrix support | Internal BT2020 constant/comment is not implemented support; gamma/gamut metadata is not supplied by this field |
| Timestamps | Caller timestamp copied unchanged | Official `Timestamp=-1` clock/pacing convenience is absent; Nodos generates its own timestamps |
| Per-frame metadata | Missing: video header metadataLength always zero, C wrapper ignores FrameMetadata | UTF-8/NUL length, max uint16 size and payload-tail roundtrip |
| Audio | Missing; C wrapper rejects Audio | FPA1 planar audio, active-channel mask, silence elision, timestamps and per-frame metadata |
| Audio header declaration | **Incorrect unused definition:** 20 bytes with wrong order/fields; official header is 24 bytes: codec, sampleRate, samplesPerChannel, channels, activeChannels, reserved1 | Correct declaration and golden bytes before adding audio; internal PCM_F32 is not official FPA1 (`0x31415046`). Audio type comment says 3 but official type is 4 |
| Video subscription | Video withheld until canonical Video subscribe | Existing native subscription gate tests |
| Audio/metadata subscription | Parsed and stored; audio unused; `Sender::sendMetadata` ignores metadata subscription | Separate protocol-control handshake from application metadata; enforce application subscription routing |
| Tally receive | Canonical legacy `Program==` and well-formed `Program=` accepted; public aggregate getters work | Existing real-TCP `test_tally` covers receive/aggregation, not outbound tally |
| Tally broadcast | **Missing:** `onTallyChanged` only logs; accept handshake sends info, not combined tally | Broadcast aggregate on change/disconnect and send initial aggregate to late joiners; use official legacy wire tokens |
| Preview mode | Request parsed into previewMode; sender never consumes it | Generate/send actual reduced VMX preview with matching header; test full and preview subscribers together |
| Suggested quality | Highest connected suggestion applied on change, Default initially SQ | Recompute after last suggestion returns to Default, disconnect and manual-to-Default switch; current callback skips Default and disconnect only logs |
| Congestion adaptation | C++ policy lowers profile after lifetime drop ratio >5%, including manual Medium/High | This is a local policy, not a protocol mandate; test one slow vs fast client and recovery. It does not imply adaptive network bitrate parity |
| Sender information | C++ greeting/custom string helper implemented; struct C API stub; helper does not escape XML attributes | Quotes/ampersands, actual Version, updated and late-joining clients |
| Connection metadata | Immediate broadcast only; not retained, clear is stub | Store/replay to future connections, clear deterministically |
| Bidirectional user metadata | Unknown incoming XML logged/discarded; `omt_send_receive` stub | Bounded receive queue and ownership/timeout contract |
| Redirect | Missing C API implementation | Official redirect metadata, existing/new clients, chained/removed redirects |
| TCP continuation/backpressure | Bounded nonblocking writes, retained partial frames, queued whole-frame eviction | Windows/Linux byte-resume, half-close, stalled/reset peer and concurrent accounting tests passed |
| Multiple senders | **Implemented in #10:** default/port 0 binds first available port 6400–6600; actual bound port exposed | Concurrent allocation, exhaustion, exact-port collision, reuse and C API tests passed |
| Network address families | IPv4 only (`sockaddr_in`, AF_INET); official sender is dual-stack | IPv4/IPv6 clients and address resolution/failover |
| DNS-SD | Host adapter/cache only; no native backend or automatic sender registration in library | `_omt._tcp`, `HOSTNAME (Source Name)`, actual port, duplicate/rename/removal, interface changes |
| Discovery server | Missing client and server | Register/de-register/replay, server-derived IP, cleanup on disconnect; optional deployment mode, not needed for every video connection |

Different sender names do not multiplex a TCP port. Each sender needs its own
listening endpoint. Automatic port allocation belongs in this library; Nodos
registers the selected endpoint and retains its sender across transient graph
stops. Its live Windows-to-Linux two-sender check selected and advertised 6400
and 6401, with both receivers delivering frames without framing/decode errors.
Existing explicit nonzero ports remain exact.

## C API inventory

Implementation reference: [`src/libomt.cpp`](../src/libomt.cpp).
Declared symbols alone must not be advertised as supported functionality.

| Functions | Status |
| --- | --- |
| `omt_send_create`, `omt_send_destroy`, `omt_send_getport` | Implemented; auto allocation tested. Creation does not automatically advertise DNS |
| `omt_send` | Video subset only; ignores FrameMetadata and rejects Audio/Metadata |
| `omt_send_connections` | Stored channel count; disconnected entries await cleanup, not an exact instantaneous subscriber count |
| `omt_send_set_info_str` | Custom helper works with escaping/version limitations above |
| `omt_send_setsenderinformation` | Stub |
| `omt_send_addconnectionmetadata` | Immediate send, not official retained/replayed behavior |
| `omt_send_clearconnectionmetadata`, `omt_send_setredirect`, `omt_send_getaddress`, `omt_send_receive` | Stubs |
| `omt_send_gettally` | Current booleans only; ignores timeout and returns false for all-off, unlike change notification semantics |
| `omt_send_getvideostatistics` | Only Frames, FramesDropped, BytesSent written; remaining fields untouched; aggregation differs from official per-media statistics |
| `omt_send_getaudiostatistics` | Stub |
| `omt_send_get_frames_sent`, `omt_send_get_frames_dropped`, `omt_send_get_bytes_sent`, `omt_send_get_profile`, `omt_send_set_quality`, `omt_send_get_recent_drops_and_reset` | Implemented custom extensions; tested in the Nodos native suite, not official protocol features |
| `omt_receive_create`, `omt_receive_destroy`, `omt_receive`, `omt_receive_send`, `omt_receive_settally`, `omt_receive_gettally`, `omt_receive_setflags`, `omt_receive_setsuggestedquality`, `omt_receive_getsenderinformation`, `omt_receive_getvideostatistics`, `omt_receive_getaudiostatistics` | All stubs |
| `omt_discovery_start`, `omt_discovery_stop`, `omt_discovery_register`, `omt_discovery_unregister`, `omt_discovery_getaddresses` | Cache/host-adapter operations only |
| `omt_discovery_on_service_found`, `omt_discovery_on_service_lost`, `omt_discovery_on_registered`, `omt_discovery_on_registration_failed` | Host callbacks implemented; do not start OS discovery by themselves |
| `omt_settings_get_string`, `omt_settings_set_string`, `omt_settings_get_integer`, `omt_settings_set_integer`, `omt_setloggingfilename` | Stubs; no settings.xml or DiscoveryServer configuration parity |
| `omt_setlogcallback` | Implemented |
| `omt_shutdown` | Destroys discovery singleton; callers must quiesce users first, not a concurrent global sender/receiver shutdown |

Receiver stubs also mean no library implementation of compressed receive,
preview decode, audio decode, automatic reconnect, multiple candidate-address
connect, or redirect following. These are separate from the Nodos receiver.

Discovery follow-ups: getAddresses currently constructs an address from the
display name, not the resolved endpoint; registration of an existing name does
not update its port; upper port bound is unchecked; findEntry returns a pointer
into mutable storage after unlocking. C discovery address storage is shared
without a lock. Address escaping, expiry, interface changes and deterministic
withdrawal need dedicated lifecycle tests, not a claim based on cached entries.

## Ordered implementation checklist

Keep changes upstream and pin reviewed commits in consumers. Do not add
transport/protocol workarounds to generated vendor trees.

- [x] Automatic ports, exact override, actual endpoint API (#10).
- [x] Review/refactor speculative send (#9); real byte oracle and bounded waits.
- [ ] Tally broadcast on change/disconnect and initial state for late joiners.
- [ ] Sender information struct API and XML escaping; retained connection metadata.
- [ ] Quality reset/recompute for Default, disconnect and manual/auto transitions.
- [ ] Alpha/flag semantics and honest unsupported-capability errors.
- [ ] Correct audio wire declarations before any audio implementation.
- [ ] VMX preview, compressed passthrough and per-frame metadata.
- [ ] C API initialization/error/timeout/ABI tests; no silent success for stubs.
- [ ] DNS host-adapter contract and portable backend ownership, actual addresses,
  settings and IPv6. Test duplicate names and endpoint reuse separately.
- [ ] Audio transport and receiver API as distinct features with independent
  official receiver/sender fixtures; do not couple them to the current video fix.
- [ ] Optional redirect and discovery-server mode with reconnect/removal fixtures.
- [ ] Cross-implementation matrix: official libomtnet sender and receiver in both
  directions, byte/layout fixtures, malformed inputs, cancellation, hot source
  changes and an endurance run. A self-loopback cannot prove complete parity.

## PR #9 evidence

The initial bot change was not accepted as-is: it wrote before checking an
expired deadline and used a fake successful codec benchmark without byte/frame
correctness. The replacement passed Windows Release and Linux ASan/UBSan CTest
(3 tests each), plus the Nodos real-VMX native suite (19 cases, no sanitizer
output). A negative control detected the original zero-budget write.

For 20,000 ordered 1 KiB TCP writes on the same Linux host, strace counted
20,000 sends + 20,000 polls before, and 20,000 sends + zero polls after.
Five alternating untraced runs had medians 47.706 ms before and 48.746 ms after.
This demonstrates fewer readiness calls in that fixture, not a proven FPS,
codec throughput or end-to-end latency improvement.
