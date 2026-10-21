# Dante interop

Researched 2026-10-18 against Audinate's own documentation. This file replaces an
earlier version that claimed Dante Controller could never display a third-party
AES67 stream — **that claim was wrong**.

## The correction

| My earlier claim | What Audinate documents |
|---|---|
| "Dante Controller only lists Dante devices; a third-party AES67 sender can never appear" | ❌ Wrong. RTP flows from non-Dante devices **do** appear in Dante Controller |
| "SAP is pointless for Dante Controller" | ❌ Wrong. Audinate explicitly supports SAP-advertised descriptors |

Audinate, [Creating AES67 or ST 2110-30 (RTP) Transmit Flows](https://dev.audinate.com/GA/dante-controller/userguide/webhelp/content/creating_aes67_or_st2110-30_rtp_transmit_flows.htm):

> RTP transmit flows (including AES67 flows) from non-Dante devices appear in blue
> at the far right of the Routing tab of the Network View, and at the bottom of the
> Available Channels list in the Device View > Receive tab. AES67 flows are
> identified by their transmit multicast IP addresses, rather than device names.

Audinate, [AES67 and SMPTE Domains](https://dev.audinate.com/GA/ddm/userguide/1.9/webhelp/content/appendix/aes67_and_smpte_domains.htm):

> Dante Controller supports RTP descriptors transmitted via SAP, and containing a
> range of specific values. Flows that are advertised this way will automatically
> appear in Dante Controller as a transmit flow.

So the mechanism the user described exists. What was missing was that our SDP had
to be well-formed for Audinate, and it was not.

## Why our flow never appeared: the SDP was malformed

Audinate, [Sample SDP Specification](https://dev.audinate.com/GA/ddm/userguide/1.9/webhelp/content/appendix/sample_sdp_specification.htm):

> Fields must appear in the order shown below...
> **Descriptors that are legal SDP but not well-formed for SMPTE will be silently
> ignored by Dante Controller and Dante devices.**

Our SDP violated that in three ways:

| Requirement | Before | Now |
|---|---|---|
| `c=` after `m=` (media level) | `c=` at session level, before `m=` | moved after `m=` |
| Field order `v,o,s,t,a,m,c,rtpmap,ptime,ts-refclk,mediaclk` | `v,o,s,c,t,m,...` | matches |
| `a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>` | `localmac=00-00-00-00-00-00` by default | `ptp=...` with the real grandmaster |

Verified output of `openaes67 sdp --refclk "ptp:00-1D-C1-FF-FE-12-34-56:0"`, and
the same bytes observed arriving through SAP:

```
v=0
o=- 1592590337 1 IN IP4 192.168.1.10
s=OpenAES67 stream
t=0 0
a=recvonly
m=audio 5004 RTP/AVP 96
c=IN IP4 239.69.1.1/32
a=rtpmap:96 L16/48000/2
a=ptime:1
a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0
a=mediaclk:direct=0
```

`a=ssrc` was dropped: Audinate ignores it, and matching their sample exactly is
worth more than carrying an extra line.

## Constraints that decide whether audio actually flows

From the same documentation:

1. **The receiver must be an AES67-enabled Dante device.** *"Devices in AES67 mode
   are able to transmit and receive AES67 multicast flows to / from non-Dante
   AES67-enabled devices."* AES67 mode lives in Device View → **AES67 Config**
   (Brooklyn II v3.9.x and later). *"Only AES67-enabled devices can receive AES67
   flows."*
2. **PTP v2 domain must be 0.** *"AES67 uses a fixed PTP V2 domain number (0)".*
3. **Our multicast address must be 239.x/16 for many receivers.** Brooklyn II,
   Dante HC and Broadway *"will only subscribe to AES67 flows in the 239.x/16
   address range"*. We use `239.69.1.1` ✓.
4. **The receiver's RTP Multicast Address Prefix must match ours**, otherwise
   *"the subscription will appear to succeed, but audio will not actually flow."*
5. **The network needs a PTP clock leader.** Dante Controller showed
   `Grand Master Clock: Unknown` with no devices present, so there was nothing to
   synchronise to and nothing to subscribe into.

## State of our implementation

| Item | State |
|---|---|
| SAP announce on 239.255.255.255 and 224.2.127.254 | ✅ measured; 30 s base interval with RFC 2974 ±1/3 jitter (`--sap-interval` to change) |
| SDP layout matching Audinate's sample | ✅ measured end to end through SAP |
| RTP at 48 kHz / 1 ms / L16 / PT 96 | ✅ 976–997 pkt/s measured |
| PTP-derived grandmaster in the SDP | ⚠️ `--refclk ptp:...` must be given explicitly; the GUI still announces `localmac` |
| PTP-locked RTP timestamps | ⚠️ implemented behind `--ptp-lock`, offset sample never established, unverified |
| Dante Controller actually displaying the flow | ❓ cannot be tested here: no Dante device on this network |

## What is still needed to prove it works

A Dante device with AES67 mode, on the same subnet, with a PTP leader. Then:

1. Enable AES67 on the device (Device View → AES67 Config → Enabled, reboot).
2. Confirm Dante Controller shows a Grand Master Clock.
3. Start `openaes67 send --sap --refclk "ptp:<gmid>:0"` with the grandmaster ID
   that Dante Controller displays.
4. The flow should appear **in blue, identified by `239.69.1.1`**, at the bottom of
   the receiver's Available Channels list.
5. Subscribe it and confirm audio.

---

## Update 2026-10-18: Dante Controller 4.18 has an explicit external-SDP dialog

Dante Controller **4.18.1.1** exposes a way to inject an SDP by hand, which is the
documented proxy mechanism:

**文件 → 外部 SDP 会话** opens **「管理外部 AES67 / ST 2110-30 会话」**, a table of
sessions with buttons 添加 / 移除 / 全部移除 / 导入 / 导出 and an **SDP 文本预览**
pane.

`添加` opens a text box where the whole SDP is pasted. The placeholder content
shows the expected shape:

```
v=0
o=- 123456 654321 IN IP4 192.168.1.100
s=Example Session
t=0 0
m=audio 5004 RTP/AVP 97
c=IN IP4 239.1.1.1/32
a=rtpmap:97 L24/48000/2
```

A ready-to-paste descriptor for our stream is generated by:

```powershell
openaes67.exe sdp --iface 192.168.1.10 --group 239.69.1.1 --port 5004 `
                  --channels 2 --rate 48000 --ptime 1 `
                  --refclk "ptp:<grandmaster-id>:0" `
                  --sdp-out openaes67-aes67.sdp
```

A copy is staged at `dist/openaes67-aes67.sdp`.

### Why this matters and what is still open

Automating the dialog from outside failed: it is custom-drawn, exposes nothing to
UI Automation, and synthetic clipboard paste into its text box does not take. A
human can add the session in seconds; an agent cannot.

Two things remain unproven:

1. Whether adding the session makes the flow visible in the routing view **without
   any Dante device present** (the file menu's session list may be independent of
   the device matrix).
2. Whether SAP advertising alone is enough on this network — DC shows
   `组播音频带宽 0 bps` even though our RTP is measurably on the wire at ~1000 pkt/s.

Both need the same thing the hardware needs: at least one Dante device, or the
external session added by hand.

### Practical note: the import dialog wants `.sdpa`

`导入` opens a native file dialog whose filter is **SDP 存档文件 (*.sdpa)** — not
`.sdp`. A descriptor saved as `.sdp` will not be selectable. Copy it to `.sdpa`
first:

```powershell
Copy-Item openaes67-aes67.sdp C:\Users\Public\openaes67-aes67.sdpa
```

Keep that path free of non-ASCII characters: Automated `SendKeys` typing into the
dialog dropped characters and ate the backslashes on a path containing 中文
directory names, producing an unusable path.

Verified working: `添加` accepts pasted SDP text and the session then appears in the
table with its **session name** and **primary multicast address** (observed:
`Example Session` / `239.1.1.1:5004` from the dialog's own placeholder).

---

## RESOLVED 2026-10-18: it was the SAP header, not the SDP

`sap.cpp` wrote the flag byte into **both** byte 0 and byte 1. RFC 2974 §6 makes
byte 1 the **authentication length** in 32-bit words, so `0x20` there claimed 128
bytes of authentication data; every compliant listener skipped into the payload and
discarded the announcement. A second bug wrote a made-up sequence counter over the
last two bytes of the 4-byte originating source, turning `192.168.1.10` into
`192.168.1.1`.

After the fix, Dante Controller lists the stream **automatically**, with no manual
step:

```
发送设备 (2)
  Example Session     01 02
  OpenAES67 stream    01 02      <- discovered over SAP
```

Both entry points were verified: `openaes67 tx --sap` and the GUI's 开始发送 button.

The lesson worth keeping: our own `sap-listen` accepted our own malformed packets
because it shared the same wrong offset arithmetic. End-to-end self-testing cannot
catch a protocol bug that both ends make; only checking bytes against the RFC or
against an independent implementation can.