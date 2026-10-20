# OMT protocol and API parity

Reviewed 2026-10-20 against libomtnet
[`7d29031`](https://github.com/openmediatransport/libomtnet/tree/7d29031ff3f355ffa1f0cd08ee24d02e258445dd)
with the media port based on libomtcpp `77d547eb663d3672bd3229e59559abd2f05b53a6`.
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

The C++ library implements an IPv4 VMX/FPA1 sender, combined tally, preview,
and a bounded reconnecting receiver (IPv4/IPv6 connections). Its public C header contains additional declarations whose
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
| Per-frame metadata | Audio send/receive and video receive preserve the NUL-terminated payload tail | Video sender still ignores FrameMetadata |
| Audio | FPA1 send/receive implemented: planar float32, 1–32 channels, silent-plane elision; independent golden bytes and official bidirectional tests | Native audio-device output and application A/V playout synchronization are outside transport scope |
| Audio header declaration | Correct 24-byte FPA1 layout and audio type 4; golden byte/offset checks | Big-endian hosts remain untested |
| Video subscription | Video withheld until canonical Video subscribe | Existing native subscription gate tests |
| Audio/metadata subscription | Audio is delivered only to subscribed connections; receiver uses separate media sockets | Legacy Sender::sendMetadata is a control broadcast, not a subscription-aware application-send API |
| Tally receive | Legacy Program== and well-formed tokens accepted; change events include transition to all-off | Zero-timeout returns state without consuming a change event |
| Tally broadcast | Combined OR broadcast on change/disconnect and initial state for new clients; Windows/Linux and official interoperability tested | Long endurance with many clients |
| Preview mode | VMX DC prefix sent per subscriber; original header geometry + Preview flag; full subscriber unchanged | Full/preview pixels and interlaced 1080 preview tested; no 8K live endurance claim |
| Suggested quality | Highest connected suggestion applied on change, Default initially SQ | Recompute after last suggestion returns to Default, disconnect and manual-to-Default switch; current callback skips Default and disconnect only logs |
| Congestion adaptation | C++ policy lowers profile after lifetime drop ratio >5%, including manual Medium/High | This is a local policy, not a protocol mandate; test one slow vs fast client and recovery. It does not imply adaptive network bitrate parity |
| Sender information | C++ greeting/custom string helper implemented; struct C API stub; helper does not escape XML attributes | Quotes/ampersands, actual Version, updated and late-joining clients |
| Connection metadata | Immediate broadcast only; not retained, clear is stub | Store/replay to future connections, clear deterministically |
| Bidirectional user metadata | Receiver sends XML with timestamp; sender exposes a bounded inbox through omt_send_receive; official interoperability passed | General sender application-metadata send and retained connection replay remain separate gaps |
| Redirect | Missing C API implementation | Official redirect metadata, existing/new clients, chained/removed redirects |
| TCP continuation/backpressure | Bounded nonblocking writes, retained partial frames, queued whole-frame eviction | Windows/Linux byte-resume, half-close, stalled/reset peer and concurrent accounting tests passed |
| Multiple senders | **Implemented in #10:** default/port 0 binds first available port 6400â€“6600; actual bound port exposed | Concurrent allocation, exhaustion, exact-port collision, reuse and C API tests passed |
| Network address families | Sender remains IPv4; receiver resolves IPv4/IPv6 candidates and tries them with bounded connect waits | IPv6 receiver path is compiled but not exercised by the IPv4 fixtures; no live DNS refresh |
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
| `omt_send` | Video subset and FPA1 audio; audio attached metadata supported; standalone Metadata still rejected |
| `omt_send_connections` | Stored channel count; disconnected entries await cleanup, not an exact instantaneous subscriber count |
| `omt_send_set_info_str` | Custom helper works with escaping/version limitations above |
| `omt_send_setsenderinformation` | Stub |
| `omt_send_addconnectionmetadata` | Immediate send, not official retained/replayed behavior |
| `omt_send_clearconnectionmetadata`, `omt_send_setredirect`, `omt_send_getaddress` | Stubs |
| `omt_send_receive` | Bounded incoming application metadata with timeout and owned C API result |
| `omt_send_gettally` | Change notification/timeout, including all-off; zero-timeout reads state without consuming the event |
| `omt_send_getvideostatistics` | Initialized output, video-only Frames/FramesDropped/BytesSent; other fields zero; aggregate active-channel totals, not lifetime or interval parity |
| `omt_send_getaudiostatistics` | Initialized audio-only counts/bytes; same active-channel limitation |
| `omt_send_get_frames_sent`, `omt_send_get_frames_dropped`, `omt_send_get_bytes_sent`, `omt_send_get_profile`, `omt_send_set_quality`, `omt_send_get_recent_drops_and_reset` | Implemented custom extensions; tested in the Nodos native suite, not official protocol features |
| `omt_receive_create`, `omt_receive_destroy`, `omt_receive`, `omt_receive_send`, `omt_receive_settally`, `omt_receive_gettally`, `omt_receive_setflags`, `omt_receive_setsuggestedquality`, `omt_receive_getsenderinformation`, `omt_receive_getvideostatistics`, `omt_receive_getaudiostatistics` | Implemented; owned per-type outputs, bounded queues, reconnect, preview/compressed flags, tally, quality, info, per-media statistics |
| `omt_discovery_start`, `omt_discovery_stop`, `omt_discovery_register`, `omt_discovery_unregister`, `omt_discovery_getaddresses` | Cache/host-adapter operations only |
| `omt_discovery_on_service_found`, `omt_discovery_on_service_lost`, `omt_discovery_on_registered`, `omt_discovery_on_registration_failed` | Host callbacks implemented; do not start OS discovery by themselves |
| `omt_settings_get_string`, `omt_settings_set_string`, `omt_settings_get_integer`, `omt_settings_set_integer`, `omt_setloggingfilename` | Stubs; no settings.xml or DiscoveryServer configuration parity |
| `omt_setlogcallback` | Implemented |
| `omt_shutdown` | Destroys discovery singleton; callers must quiesce users first, not a concurrent global sender/receiver shutdown |

Receiver limitations: no redirect following, background DNS refresh or discovery
subscription. Creation resolves addresses synchronously. At most 16 resolved
candidates are tried sequentially, with bounded per-address connect waits. Decoder
limits are 16–7680 pixels wide and 16–4320 high; interlace supports 480/576/1080.
A P216 preview preference falls back to 8-bit UYVY. See the ownership and test
contract in [MEDIA-PORT-VALIDATION.md](MEDIA-PORT-VALIDATION.md).

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
- [x] Tally broadcast on change/disconnect and initial state for late joiners.
- [ ] Sender information struct API and XML escaping; retained connection metadata.
- [ ] Quality reset/recompute for Default, disconnect and manual/auto transitions.
- [ ] Alpha/flag semantics and honest unsupported-capability errors.
- [x] Correct FPA1 audio wire declaration and golden-byte test.
- [x] VMX preview and compressed receive.
- [ ] Compressed send passthrough and video sender per-frame metadata.
- [ ] C API initialization/error/timeout/ABI tests; no silent success for stubs.
- [ ] DNS host-adapter contract and portable backend ownership, actual addresses,
  settings and IPv6. Test duplicate names and endpoint reuse separately.
- [x] FPA1 audio and bounded receiver C API with official bidirectional fixtures.
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
