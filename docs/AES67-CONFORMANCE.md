# AES67 conformance analysis

What this document is: a clause-by-clause comparison between **AES67-2018**
(plus the parts of **ST 2110-30** that extend it) and what this repository
actually does, with file-level evidence. It is not a certification, and it does
not claim that a device passing these checks will pass an AES67 plugfest.

What it is for: making the gaps explicit. A conformance table that says
"conformant" everywhere is marketing; the useful ones name what is missing and
what breaks because of it.

Evidence refers to the tree at the commit this file was added to. Line numbers
drift; the function and constant names are the stable anchors.

---

## 1. Summary

| Area | Requirement source | Verdict |
|---|---|---|
| Media clock signalling | AES67 §7.1, §8.2 (`ts-refclk`, `mediaclk`) | **Conformant** for PTP; `localmac` fallback is the standard's own fallback |
| Sampling rate | AES67 §7.2 (48 kHz mandatory) | **Conformant**, with extra rates (8 k – 384 k) |
| Packet time | AES67 §7.3 (1 ms mandatory, 125 µs optional) | **Conformant** for both; arbitrary ptime 0.01 – 100 ms accepted |
| Encoding | AES67 §7.4 (L16 mandatory, L24 optional) | **Conformant** for both |
| Channels per stream | AES67 §8.1 (8 per stream) vs ST 2110-30 (more, with limits) | **Deviates from AES67**: up to 64 |
| RTP packetisation | RFC 3550, AES67 §7.5 | **Conformant** |
| SDP | AES67 §8.2, RFC 4566, RFC 4570, RFC 7273 | **Conformant**; attribute order tuned for Audinate's parser |
| SAP discovery | AES67 §9, RFC 2974 | **Conformant** |
| PTP | AES67 §10, IEEE 1588-2008 | **Partial**: slave-only, E2E only, software timestamps, no BMCA |
| RTCP | AES67 §7.5 (optional) | Implemented (SR + SDES CNAME, RR parsing) |
| QoS / DSCP marking | AES67 §12, ST 2110-21 | **Implemented**: audio EF (46), PTP CS6 (48) |
| Source-specific multicast | RFC 4570 / RFC 4607 | **Advertising only**: the sender announces `a=source-filter`, the receiver still joins ASM |
| Seamless redundancy | ST 2022-7 | **Not implemented** (out of AES67 scope, required by 2110) |
| IPv6 | ST 2110-10 (separate) | **Not implemented** |
| Security (SAP authentication, SRTP) | out of scope | **Not implemented** (AES67 has none by design) |

The entry that still bites on a real network is **source-specific multicast**; the
rest are either optional or only matter for a fully ST 2110 plant.

---

## 2. Media clock (§7.1, §8.2)

**Requirement.** A sender's SDP must state how the RTP timestamps relate to a
clock: `a=ts-refclk` names the reference (PTP grandmaster or a local clock), and
`a=mediaclk` says whether media time is derived from it (`direct=0`) or offset.

**Implementation.** `buildSdp()` in `src/sdp.cpp`:

```
a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>     when --ptp / --ptp-lock / --refclk ptp:…
a=ts-refclk:localmac=AA-BB-CC-DD-EE-FF           otherwise
a=mediaclk:direct=0
```

**Verdict.** Conformant. Two notes worth keeping:

- `localmac` is not a defect. AES67 explicitly allows a sender that is not
  PTP-locked to declare a local clock; the MAC makes the announcement unique.
  What *was* a defect is emitting `localmac` unconditionally while announcing
  over SAP — Dante Controller then showed the flow and refused to lock to it.
  That is why the clock reference is now derived from the actual configuration.
- `a=clock-domain:PTPv2 <n>` is emitted alongside, which is a Ravenna-ism rather
  than an AES67 requirement. Receivers that do not know it ignore it; those that
  do use it to avoid cross-domain pairing.

## 3. Sampling rate and packet time (§7.2, §7.3)

**Requirement.** 48 kHz is mandatory; other rates are optional. A conformant
implementation must support 1 ms packet time, and *may* support 125 µs.

**Implementation.** `StreamConfig` defaults to 48 000 Hz and 1.0 ms
(`include/openaes67/config.h`); validation accepts 8 000 – 384 000 Hz and
0.01 – 100 ms (`src/config.cpp`). `framesPerPacket()` is
`rate * ptime / 1000` rounded to nearest, so 48 kHz at 0.125 ms is 6 frames.

**Verdict.** Conformant, and wider than required. The extra range is deliberate:
AES67 profiles beyond the base (and ST 2110-30) use 96 kHz, and 44.1 kHz appears
in Ravenna deployments.

**Caveat.** The 1 ms default is what interoperates. Everything else is a
negotiation the receiver is free to refuse, and Dante devices typically do.

## 4. Encoding (§7.4)

**Requirement.** L16 (16-bit linear, big-endian) is mandatory; L24 (24-bit) is
optional. RTP timestamps count samples at the stream rate.

**Implementation.** `packPayload()` / `unpackPayload()` in `src/rtp.cpp`.

- L16: big-endian `int16` pairs, 2 bytes per sample.
- L24: **the 16-bit sample occupies the upper two bytes and the low byte is
  zero** — that is, a full-scale 16-bit sample maps to a full-scale 24-bit
  sample.

**Verdict.** Conformant on the wire: 3 bytes per sample in network order, `a=rtpmap`
names `L24`, and the payload length is `frames × channels × 3`.

**Deviation worth stating out loud.** The internal pipeline is `int16`. An L24
stream therefore carries **16 bits of real resolution** in a 24-bit container;
the low 8 bits are always zero. This is not a bug — it is the honest consequence
of a 16-bit engine — but any claim of "24-bit audio" would be false. The
resampler and the ASIO driver both stay at 32-bit-for-ASIO / 16-bit-internal for
the same reason.

Receivers cannot tell the difference (the low byte being zero is legal audio
data), so this harms nobody's interoperability; it only matters if someone
expects L24 to buy headroom. It does not.

## 5. Channels per stream (§8.1)

**Requirement.** AES67-2018 limits a stream to **8 channels** at 48 kHz
(§8.1). ST 2110-30 relaxes this for its own profiles, with packet-time and
channel-count combinations spelled out.

**Implementation.** `StreamConfig::channels` is validated 1 – 64, default 2.
`--channels 64 --ptime 1` is accepted and announced as `a=rtpmap:96 L16/48000/64`.

**Verdict.** **Deviates from AES67.** A 64-channel stream is an ST 2110-30 style
flow. Devices that enforce AES67's 8-channel cap will reject it — and Dante
Controller's own flows are 8 channels, so a 64-channel *transmit* flow is
unusual on a Dante network and will not be shown as a Dante flow.

**Why it is still there.** The 64×64 sound card exists so a DAW can address 64
channels; internally that is a single shared ring, not a single RTP flow. The
`bridge` command is what exposes 64 channels. For AES67 interop, use
`--channels 8` (or the receiver's flow size) and multiple streams.

**Handling.** The program prints a note at startup when `channels > 8`, naming the
AES67 limit, the ST 2110-30 alternative and the fix (`--channels 8` with several
streams). It does not refuse the configuration — a 64-channel flow is legitimate
on a network that expects it, and this project's own sound card is the reason the
option exists.

## 6. RTP packetisation (RFC 3550, §7.5)

**Requirement.** RTP version 2, PT in the dynamic range, monotonically
increasing sequence, timestamps advancing by the frame count, SSRC unique per
source.

**Implementation.** `writeRtpHeader()` writes exactly 12 bytes: V=2, P=0, X=0,
CC=0, M as configured, PT 96 (L16) or 97 (L24). Sequence and timestamp advance in
`RtpPacketizer` (`include/openaes67/rtp.h`); SSRC is random per run unless
`--ssrc` is given, per RFC 3550's recommendation.

**Verdict.** Conformant. No CSRC list, no header extension, no padding — all
optional. The marker bit is not used for talk-spurt signalling, which is correct
for continuous audio.

## 7. SDP (§8.2)

**Requirement.** SDP per RFC 4566 with AES67's mandatory attributes: `c=` naming
the multicast group, `a=rtpmap` with the exact encoding name, `a=ptime`,
`a=ts-refclk`, `a=mediaclk`. `a=source-filter` (RFC 4570) is recommended for SSM.

**Implementation.** `buildSdp()` emits, in this order:

```
v=0
o=- <ssrc> <session-version> IN IP4 <iface>
s=OpenAES67 stream
t=0 0
a=sendonly | a=recvonly
m=audio <port> RTP/AVP <pt>
c=IN IP4 <group>/32
a=source-filter: incl IN IP4 <group> <iface>
a=rtpmap:<pt> L16|L24/<rate>/<channels>
a=ptime:<ms>
a=ts-refclk:…
a=mediaclk:direct=0
a=sync-time:0
a=framecount:<frames>
a=clock-domain:PTPv2 <domain>
a=mid:1
```

**Verdict.** Conformant. The ordering comment in the source is worth preserving:
Audinate's sample SDP specifies v, o, s, i, t, a, m, c, rtpmap, ptime, ts-refclk,
mediaclk, and "legal SDP that is not well-formed for SMPTE is silently ignored
by Dante Controller". Putting `c=` at session level before `m=` (the previous
behaviour) is legal SDP and invisible to Dante — which is exactly the kind of
failure that looks like a network problem.

`a=recvonly` matters too: announcing a receiving stream as `sendonly` made Dante
Controller list a flow that could never be subscribed to.

## 8. SAP discovery (§9, RFC 2974)

**Requirement.** Announcements are sent to the SAP multicast group (conventionally
239.255.255.255:9875) with an `application/sdp` payload; a deletion packet
(identical `msg id hash`, TTL 0 semantics) withdraws the session.

**Implementation.** `src/sap.cpp` announces to **both** `239.255.255.255` and
`224.2.127.254` on port 9875 with TTL 255, re-announces on a timer, and sends the
deletion packet on stop.

**Verdict.** Conformant. Dual-group transmission is a practical choice: the IPv4
"all SAP" group is 239.255.255.255, but some implementations listen on the older
224.2.127.254. Announcing on both costs one packet every few seconds and removes
a whole class of "why does nothing see my stream" reports.

## 9. PTP (§10, IEEE 1588-2008)

**Requirement.** AES67 requires a PTPv2 slave that can lock to a grandmaster;
PTP is the media clock reference.

**Implementation.** `src/ptp.cpp` implements a **slave only**: Sync/Follow_Up for
t1, Delay_Req/Delay_Resp for the path delay (E2E mechanism), offset and frequency
estimation, and `PtpMediaClock` to derive RTP timestamps without reading the
clock per packet.

**Verdict.** **Partial.** Specifically:

| Aspect | State | Consequence |
|---|---|---|
| Message types | Sync, Follow_Up, Delay_Req/Resp, Announce (parsed for identity) | A P2P (peer-delay) master will not be tracked |
| Delay mechanism | E2E only | Same |
| Best master clock | none (slave only) | Correct for a device that is never a grandmaster |
| Timestamps | software (`QueryPerformanceCounter`-based arrival times) | ~0.2 ms accuracy with a constant bias; not suitable for <1 µs alignment |
| Announce handling | identity used; `clockClass`/`priority1` not used for selection | If two masters appear, the first seen wins |

That last row is a real limitation on a plant with redundant grandmasters:
without BMCA the slave may lock to the wrong one. It has never been observed on a
Dante network (Dante elects a leader itself and there is normally one), but it is
the honest answer to "is this 1588 conformant".

## 10. RTCP (§7.5, optional)

**Requirement.** AES67 does not require RTCP. It may be used for sender/receiver
reports.

**Implementation.** `src/rtcp.cpp` builds SR + SDES(CNAME) compound packets and
parses SR/RR/CNAME. `--rtcp` enables it on port+1.

**Verdict.** Implemented, optional, and off by default — which is the right
default for a network where most AES67 devices do not send RTCP.

## 11. QoS / DSCP marking

**Requirement.** AES67 §12 and ST 2110-21 expect audio to be marked DSCP EF (46)
and PTP to be marked CS6/CS7 so switches prioritise them. Without marking, a
congested link drops audio and PTP equally with bulk traffic.

**Implementation.** `setSocketDscp()` in `src/net.cpp` sets `IP_TOS`, and the two
sockets that matter are marked:

| Socket | DSCP | TOS byte | Rationale |
|---|---|---|---|
| Audio (sender) | EF 46 | 0xB8 | AES67 §12 / ST 2110-21 |
| PTP | CS6 48 | 0xC0 | clock above audio |

SAP is deliberately left unmarked: it is low-rate control traffic whose loss is
recoverable by the next announcement, and marking it would put it in the same
queue as audio.

**Verdict.** **Implemented.** Windows maps TOS to the 802.1p priority in the
VLAN tag itself, so no separate 802.1Q call is needed.

**Verification.** `verifyDscpSupport()` opens a throwaway socket, sets a value,
reads it back with `getsockopt`, and reports what the platform did. The self-test
asserts the round trip and prints it:

```
[PASS] net: dscp marking survives a round trip
[INFO] dscp: ef=46 cs6=48
```

That is stronger than "we called setsockopt": a driver or a policy layer that
ignores the option would fail the assertion.

**What it does not prove.** That the packet actually leaves with that TOS on the
wire. Verifying that needs a capture (`pktmon`, Wireshark) on the sending host;
the round trip proves the socket is configured, not the NIC's behaviour.

## 12. The two smaller gaps

**Source-specific multicast.** The sender advertises
`a=source-filter: incl IN IP4 <group> <source>` (RFC 4570), but the receiver joins
with `IP_ADD_MEMBERSHIP`, i.e. any-source multicast. On an IGMPv2 network this is
harmless; on an SSM-only (IGMPv3, RFC 4607) network the join would need
`IP_ADD_SOURCE_MEMBERSHIP`. So the advertisement promises something the receive
path does not use.

**IPv6 and ST 2022-7.** Neither is implemented. Both are ST 2110 requirements
rather than AES67 ones, and both are large. Not planned.

## 13. Ranked gaps

Ordered by (impact on a real Dante/AES67 network) ÷ (effort). The first two items
of the previous revision — DSCP marking and the `--channels > 8` note — are done;
they are described in §11 and §5 rather than here.

1. **SSM join when the SDP carries `a=source-filter`** — parse the attribute in
   the receiver and use `IP_ADD_SOURCE_MEMBERSHIP`. Only matters on SSM networks,
   but the current state is inconsistent with what we advertise. *Medium.*
2. **PTP `Announce`-based master selection** (compare `clockClass`, `priority1`,
   `stepsRemoved`) — needed only when two grandmasters are visible. *Medium.*
3. **Hardware/NDIS timestamps for PTP** — would move accuracy from ~0.2 ms to
   tens of microseconds, but requires driver work and a NIC that supports it.
   *Large, and out of scope for a user-mode project.*
4. **ST 2022-7 seamless redundancy** — two NICs, sequence-merged duplicate
   streams. *Large.*

## 14. How to verify the claims in this document

```bat
:: what the SDP actually says
openaes67.exe sdp --channels 8 --ptime 1 --codec L24 --refclk ptp:00-0E-C6-FF-FE-37-38-07:0

:: what the sender announces over SAP, and whether a receiver sees it
openaes67.exe sap-listen --seconds 10

:: whether PTP is actually locked, and to whom
openaes67.exe ptp --iface <ip> --seconds 10

:: the environment-level checks (interface, multicast, SAP receive, ASIO, PTP)
openaes67.exe doctor --iface <ip>
```

The conformance statements above are covered by assertions in
`tests/selftest.cpp` where they are testable (SDP attribute presence and ordering,
L16/L24 round trips, ptime-to-frames arithmetic, PTP header parsing, SAP datagram
layout). Statements that depend on the network (§9 timestamps, §11 QoS) are not
asserted, and are marked as such here rather than asserted to.

## 15. Sources

- **AES67-2018**, *Audio Engineering Society Standard — Interoperable
  Audio-over-IP Interoperability*.
- **ST 2110-30:2021**, *Professional Media Over Managed IP Networks — PCM
  Digital Audio*.
- **RFC 3550** (RTP), **RFC 4566** (SDP), **RFC 2974** (SAP), **RFC 4570**
  (source filters), **RFC 7273** (clock signalling), **RFC 4607** (SSM).
- **IEEE 1588-2008**, *Precision Clock Synchronization Protocol*.
- Audinate, *Dante and AES67* and the *Sample SDP Specification* — the attribute
  ordering constraint in §7 comes from there, not from AES67 itself.
