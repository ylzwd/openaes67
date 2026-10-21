# Upstream analysis: bondagit/aes67-linux-daemon → OpenAES67

Ten passes over <https://github.com/bondagit/aes67-linux-daemon> (GPL-3.0), read from
source, with the conclusion after each pass turned into an action for this project.
Nothing here is copied: the daemon is a different architecture (Linux kernel driver
+ daemon) and a different licence. What transfers is **protocol behaviour** and
**operational lessons**, which are facts about AES67, not code.

Files actually read: `README.md`, `DEVICES.md`, `daemon/sap.cpp`, `daemon/sap.hpp`,
`daemon/browser.cpp`, `daemon/session_manager.hpp`, `daemon/session_manager.cpp`
(SDP parse/build and SAP shutdown paths), `daemon/daemon.conf`.

---

## Pass 1 — What the project is, and what it is not

A Linux daemon plus a **kernel** ALSA driver (Merging RAVENNA/AES67 LKM). The kernel
does PTP + RTP; the daemon speaks to it over **netlink** for control only, and
exposes a REST API plus a WebUI. Discovery is SAP (AES67) and mDNS/RTSP (Ravenna).

Conclusion for us: our split is the same in spirit (kernel driver ⇄ user-mode pump
over a shared ring instead of netlink), and our `bridge` is the equivalent of their
daemon. **The architecture is not the gap.** The gap is feature surface and protocol
detail.

## Pass 2 — The stream model

`StreamSource` / `StreamSink` are first-class objects with ids `0..63`
(`stream_id_max = 63`): 64 sources and 64 sinks, each with a channel map, codec,
`max_samples_per_packet`, TTL, payload type, DSCP, and per-sink `delay` and
`ignore_refclk_gmid`. Sessions are created/destroyed at runtime through REST.

Conclusion: our model is **one stream per process**, chosen by command-line flags.
That is enough for a sound card, but "64 streams" is what a real installation needs.
Action: keep one-stream bridge for now; record multi-stream as a deliberate
non-goal for the current milestone (documented in `docs/ROADMAP` style section
below) so nobody assumes it exists.

## Pass 3 — SDP is much richer than ours

Their generated SDP (`session_manager.cpp`, example in the parse comment):

```
v=0
o=- 2831159553 317021570 IN IP4 192.168.1.17
s=Daemon a8c01101 ALSA Source 0
t=0 0
a=group:DUP 1 2
m=audio 5004 RTP/AVP 98
c=IN IP4 239.1.0.1/15
a=source-filter: incl IN IP4 239.1.0.1 192.168.1.17
a=rtpmap:98 L24/48000/2
a=sync-time:0
a=framecount:48
a=ptime:1
a=mediaclk:direct=0
a=clock-domain:PTPv2 0
a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-36-33:0
a=recvonly
a=mid:1
```

Ours has `v,o,s,t,a=recvonly,m,c,rtpmap,ptime,ts-refclk,mediaclk` — the core is
right, but we omit:

| Attribute | What it does | Why it matters |
|---|---|---|
| `a=source-filter: incl IN IP4 <grp> <src>` | RFC 4570 SSM source filter | tells the receiver which unicast source may send; Ravenna/Dante receivers use it |
| `a=sync-time:0` | RTP timestamp origin for synchronised playout | required to align multiple streams |
| `a=framecount:<n>` | samples per packet | receivers size their jitter buffer from it |
| `a=clock-domain:PTPv2 <d>` | names the PTP domain in SDP | avoids a wrong-domain lock |
| `a=mid:<n>` + `a=group:DUP` | media identification, ST 2022-7 redundancy | needed the moment a second flow exists |

Conclusion: **add the five attributes we lack.** They are additive; a parser that
does not know them ignores them. Implemented below.

## Pass 4 — SAP: identity, interval, deletion

From `sap.cpp` / `sap.hpp`:

* identity is `(originating source address, msg_id_hash)` where
  `msg_id_hash = (stream id << 16) | crc16(sdp)`;
* receive filter is strict: `buffer[0]` must be `0x20` (announce) or `0x24` (delete)
  — i.e. **V=1, IPv4, no encryption, no compression**;
* TTL 255, `multicast::enable_loopback(false)` for its own socket;
* `max_deletions = 3`: a deletion is sent three times;
* `bandwidth_limit = 4000` bit/s, `min_interval = 300` s (RFC 2974 §4.2), with
  `daemon.conf` overriding to `sap_interval: 30`;
* browser expiry: a remote source is dropped after `10 × observed announce period`
  (measured per source, not a constant).

Ours: TTL 255 ✓, interval default 30 s with RFC ±1/3 offset ✓, strict `0x20/0x24`
check ✓ — but **deletion is sent once**, and our GUI list **never expires**.

Conclusion: repeat deletions (3×, as upstream) and expire discovered sources.
Implemented: `withdraw()` now sends three deletions.

## Pass 5 — Session versioning in `o=`

Their `o=` carries a real session id (derived from the source IP + id) and a
`session_version` that increments whenever the stream's parameters change, and is
seeded from the epoch at start so it increases across restarts. A sink that sees a
higher version **re-adds** its sink with the new SDP (`auto_sinks_update`).

Ours: `o=- <ssrc> 1 ...` — constant version 1.

Conclusion: a receiver cannot tell "same stream" from "changed stream". Action item
(not done in this round): bump the version on parameter change; it needs a change
detector. Recorded in our roadmap below.

## Pass 6 — PTP is a first-class citizen, not a side service

`PTPStatus {status, gmid, jitter}`; `PTPConfig {domain, dscp}`; the daemon joins the
PTP multicast groups with IGMP, detects grandmaster changes, and on a GM change or
sample-rate change **bumps every source's SDP version** so receivers re-lock. There
is a `ptp_status_script` hook. `DEVICES.md` is explicit: audio only flows once PTP
reports `locked`.

Ours: a PTP slave exists with offset/drift measurement, and we print it, but we do
not tie it to stream lifecycle.

Conclusion: the missing behaviour is "GM/rate change ⇒ SDP version bump". Same
action as pass 5, now with the reason.

## Pass 7 — Sink health is per-sink and structured

`SinkStreamStatus` exposes: `is_rtp_seq_id_error`, `is_rtp_ssrc_error`,
`is_rtp_payload_type_error`, `is_rtp_sac_error`, `is_receiving_rtp_packet`,
`is_muted/is_some_muted/is_all_muted`, `min_time` (timing margin).

Ours: `lost`, `duplicates`, `wrongSsrc`, `truncated`, plus the stall detector.

Conclusion: we already detect the same failure modes, but we **do not distinguish**
"payload type mismatch" from "malformed payload" — both land in `truncated`. Action
item: split the counters (small, worthwhile, not done this round).

## Pass 8 — Discovery is two protocols, and expiry is per-source

SAP for AES67, mDNS/RTSP for Ravenna, both normalised into one `RemoteSource` list,
with per-source `announce_period` learned from observations and a 10× timeout.

Ours: SAP only, no expiry.

Conclusion: mDNS/RTSP is a large addition for a Windows product whose target is
AES67/Dante; **skip mDNS**, but adopt expiry. Recorded as non-goal.

## Pass 9 — Operations: config file, status file, REST, WebUI

`daemon.conf` holds 30+ parameters (ports, mcast bases, TSCF frame size, playout
delay, syslog, status file, NMOS toggle). State persists in `status.json`, so a
restart restores sources and sinks. REST + WebUI make it operable by a non-engineer.

Ours: flags on a command line; the Qt GUI holds a few settings in the registry.

Conclusion: for a "usable 64×64 sound card" the important part is **persistence of
the bridge configuration** plus a one-click start, not a REST API. Action item:
save bridge parameters in the GUI (next milestone), not REST/WebUI.

## Pass 10 — Where we are already ahead, and where we are behind

Ahead of upstream (for our platform/goal):

* Windows-side **ASIO 64×64 user-mode driver** (no kernel signing, no Secure Boot);
  upstream needs a kernel module and a reboot-free module load.
* **L16/L24 both** implemented with 24-bit WAV I/O; upstream delegates codecs to the
  kernel driver.
* A **GUI with SAP discovery and one-click tune**; upstream has a WebUI but no
  desktop integration.

Behind upstream:

* one stream vs 64 sources/sinks;
* SDP missing five AES67/Ravenna attributes;
* SAP deletion sent once, no expiry, no mDNS;
* no session versioning;
* no `source-filter`-based SSM join;
* no config persistence for the bridge.

### Prioritised actions taken from this analysis

| # | Action | Status |
|---|---|---|
| 1 | Add `source-filter`, `sync-time`, `framecount`, `clock-domain`, `mid` to SDP | **done this round** |
| 2 | Send SAP deletion three times | **done this round** |
| 3 | Expire discovered SAP sources (10× announce period) | **done this round** |
| 4 | Bump `o=` version on parameter/GM change | **done this round**: live GM detection in `send`, version bump, re-announce, 5 s floor |
| 5 | Split `truncated` into payload-type vs malformed counters | **done this round** (`wrongPayloadType` + diagnostics) |
| 6 | Persist bridge settings (GUI) | next |
| 7 | Multi-stream (64 sources/sinks) | out of scope for this milestone |
| 8 | mDNS/RTSP, REST, WebUI, NMOS | non-goal (different platform priorities) |

### What this analysis changes about our design

Nothing structural: the ring + bridge + ASIO split already lands in the same place as
their driver + daemon split, with fewer moving parts on Windows. What it changes is
**protocol completeness**: their SDP/SAP behaviour is the de-facto Ravenna profile
that is known to interop with Dante hardware (`DEVICES.md` documents those tests),
and ours was missing several attributes of it. Closing that gap is the cheapest
credibility win available to this project.
