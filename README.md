> **English** · [简体中文](README.zh-CN.md)

<img src="logo.png" alt="OpenAES67" width="128" align="right">

# OpenAES67

An open-source AES67 network audio implementation for Windows, including a
**64-in / 64-out virtual sound card** that any ASIO application can open.

**MIT licensed · no activation · no licence key · no account · no phone-home.**

RTP/SDP, SAP, PTP and the audio I/O are written in C++17 from the public standards
(AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588). There is no third-party
vendor code or binary in this repository, and the project is not affiliated with
any vendor.

**60-second path to a working sound card**

```powershell
.\build.bat                                                   # build CLI + self-test
.\asio_driver\build-asio-driver.bat                           # build the 64x64 ASIO card
.\asio_driver\register-asio-driver.bat                        # register it (elevated)
.\build\openaes67.exe doctor --iface <your-wired-NIC-ip>      # check the machine
.\build\openaes67.exe bridge --iface <ip> --channels 64 --ptime 1 --shm OpenAES67Audio --sap
# then open the ASIO device "OpenAES67 ASIO 64x64" in the DAW at 48 kHz
```

---

## Contents

| | Section | 中文 |
|---|---|---|
| 1 | [What problem does it solve](#1-what-problem-does-it-solve) | 项目解决什么问题 |
| 2 | [Features](#2-features) | 主要功能 |
| 3 | [Installation](#3-installation) | 安装方法 |
| 4 | [Usage](#4-usage) | 使用方法 |
| 5 | [Input and output examples](#5-input-and-output-examples) | 输入输出示例 |
| — | [What Dante Controller shows](#what-dante-controller-shows-and-what-it-never-will) | 在 Dante Controller 里的可见性 |
| — | [Layout](#layout) · [Limitations](#honest-limitations) · [Roadmap](#roadmap) | 目录结构 · 已知限制 · 路线图 |
| — | [CLI reference](#cli-reference) | 命令行参考 |

完整中文版：[`README.zh-CN.md`](README.zh-CN.md)。

---

## 1. What problem does it solve

Windows has no built-in way to speak AES67. That leaves three practical problems,
and all three are solved here:

**Bring Dante / AES67 audio into a Windows program.** A Dante device in AES67 mode
sends multicast RTP, but Audition, Reaper or Pro Tools cannot open a multicast
stream. OpenAES67 receives it and presents it as a sound card with 64 input
channels, so the DAW records it like any other interface.

**Get a Windows program's audio onto an AES67 network.** The other direction is the
same problem reversed: the mixer or player can only see a sound card, so the
project provides 64 output channels that are transmitted as AES67.

**Do it without a commercial virtual sound card.** Existing options are licensed
products or vendor-specific. This project is MIT, runs entirely locally, and every
behaviour can be traced to a published standard or a reproducible measurement in
[`docs/FINDINGS.md`](docs/FINDINGS.md).

What it deliberately does **not** do: it is not a Dante device. See
[What Dante Controller shows](#what-dante-controller-shows-and-what-it-never-will)
for why, and what the equivalent workflow is.

---

## 2. Features

### Protocols

| Feature | Status | Notes |
|---|---|---|
| AES67 RTP (L16 / L24, 1 ms packets) | ✅ | multicast, big-endian on the wire; 8 kHz – 384 kHz, 1–64 channels |
| SDP generation and parsing | ✅ | `a=rtpmap`, `a=ptime`, `a=ts-refclk`, `a=mediaclk`, `a=source-filter`, `a=sendonly` |
| SAP announcement and discovery | ✅ | RFC 2974, TTL 255, RFC jitter, deletion on exit, repeats every interval |
| PTPv2 slave | ✅ | offset 0.2 ms measured; frequency estimate feeds the rate controller |
| RTCP | ✅ | `--rtcp`: SR every 5 s, RR with loss and jitter |
| Jitter buffer with reordering | ✅ | duplicate/late detection, loss counting, SSRC latching |

### Audio path

| Feature | Status | Notes |
|---|---|---|
| WAV source and sink | ✅ | bit-exact round trip (96 000 frames, 0 samples differ) |
| WASAPI capture and render | ✅ | shared and **exclusive** mode |
| Rate controller (resampling + PI) | ✅ | tracks ±300 ppm injected error |
| Closed loop (PTP drives playback) | ✅ | `recv --ptp` |
| 64 × 64 ASIO sound card | ✅ | `asio_driver/`: 64 in / 64 out at 48 kHz, 256-frame buffer |
| WDM/PortCls driver | ⚠️ | toolchain proven (build + sign + Inf2Cat); not loadable with Secure Boot on |

### Tooling

| Feature | Status | Notes |
|---|---|---|
| Qt 6 GUI | ✅ | live buffer/ppm/packets/loss/underruns, tray, start with Windows |
| `doctor` environment check | ✅ | interface, multicast, SAP receive, ASIO registration, PTP |
| `probe` format detection | ✅ | derives channels/codec/ptime from a live address, no SDP needed |
| `bench` + baseline gate | ✅ | 8 cases; `--baseline` fails on a >15 % regression |
| Config file | ✅ | `--config FILE`; unknown keys are startup errors |
| Self-test | ✅ | **302 assertions**, no external framework |

### Measured (reproducible, see `docs/FINDINGS.md`)

| Measurement | Result |
|---|---|
| WAV round trip | 96 000 frames, **0 samples differ** |
| `rx` integrity | 3000 packets / 144 000 frames, **0 lost** |
| PTP offset accuracy | 0.2 ms, peak-to-peak 0.16 ms |
| PTP frequency (standalone) | +307.4 ppm measured for +300 injected (2.5 % error) |
| 12-minute 64-channel duplex soak | 135 008 callbacks, 34.5 M frames, 0 lost, 0 uplink loss, flat memory and handles |
| Jitter buffer in that soak | mean 1435 / p95 1459 / **max 1468** frames against a 1440 target |
| Receive-path queue (optimisation) | 2276 Mframes/s at 2 ch, 697 Mframes/s at 64 ch (+71 % / +76 %) |

---

## 3. Installation

### Requirements

- Windows 10 or 11 (x64)
- Visual Studio 2022 Build Tools with the C++ workload, plus the Windows 10/11 SDK
- Qt 6 (only for the GUI)
- Python 3 (only for the tools under `tools/`)
- Windows Driver Kit (only for the WDM driver; the ASIO sound card does not need it)

### Build the command-line tools

```powershell
.\build.bat
```

This compiles the CLI, the self-test and the Python-free core, then runs the
project's claim checks. A CMake build is equivalent:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Verify:

```powershell
.\build\openaes67.exe help
.\build\selftest.exe        # 302 assertions, must print ALL PASS
```

### Build and register the 64×64 sound card

The driver script only needs the MSVC toolset; the WDK is not involved because an
ASIO driver is a user-mode COM DLL.

```powershell
.\asio_driver\build-asio-driver.bat          # produces asio_driver\out\openaes67_asio64.dll
.\asio_driver\register-asio-driver.bat       # needs an elevated prompt
```

Or do it from the GUI: the **注册 ASIO 驱动** button asks Windows for elevation and
runs the same script. Registration writes:

```
HKLM\SOFTWARE\ASIO\OpenAES67 ASIO 64x64   -> CLSID {7C1F2E64-9A3B-4C5D-8E7F-0A1B2C3D4E5F}
HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32 -> path to the DLL
```

Unregister with `.\asio_driver\unregister-asio-driver.bat`.

### Build the GUI (optional)

```powershell
cmake -S . -B build-qt -G "Visual Studio 17 2022" -A x64 -DOPENAES67_WITH_GUI=ON
cmake --build build-qt --config Release
```

The GUI needs the Qt 6 MSVC kit on `CMAKE_PREFIX_PATH`; see
[`docs/AUDIO-DEVICES.md`](docs/AUDIO-DEVICES.md) if CMake cannot find Qt.

### Check that the machine is ready

```powershell
.\build\openaes67.exe doctor --iface 192.168.1.10
```

`doctor` exits 0 when nothing blocking was found and 1 otherwise, so an installer
can gate on it. Everything it reports is explained in section 5.

### Optional: start with Windows

```powershell
.\build-qt\Release\openaes67_gui.exe --autostart-register
```

Writes one `HKCU\...\CurrentVersion\Run` entry that starts the GUI in the mode
currently configured (no elevation needed). Remove it with
`--autostart-unregister`.

---

## 4. Usage

### Commands

| Command | What it does |
|---|---|
| `doctor` | check the environment (interface, multicast, SAP receive, ASIO registration, PTP) |
| `probe` | listen on one multicast address and report the format it carries |
| `info` | print the resolved stream parameters |
| `sdp` | print the SDP this configuration would announce |
| `tx` | transmit a test tone or a WAV file (no audio device required) |
| `rx` | receive to a WAV file (no audio device required) |
| `send` | capture the PC's output (WASAPI loopback) and transmit it |
| `recv` | receive and play to a device, a WAV file or a shared section |
| `bridge` | both directions at once through the shared ring (sound-card mode) |
| `capture`, `capture-in`, `play` | diagnostics: loopback to WAV, input endpoint to WAV, WAV to device |
| `sap-listen` | list the streams announced over SAP |
| `ptp` | measure offset and drift against a grandmaster |
| `shm-read` | drain a shared section and report what arrived |
| `list-devices` | enumerate WASAPI endpoints and ASIO drivers |
| `bench` | measure the hot paths; with `--baseline`, gate on them |
| `version` | print the version and exit |
| `help` | print the command and option summary |

### Workflow 1 — send this PC's audio to the network

```powershell
# What the machine is playing -> AES67, announced over SAP so controllers see it
.\build\openaes67.exe send --iface 192.168.1.10 --group 239.69.1.2 --port 5006 --sap
```

A Dante receiver (set to AES67 mode) can then subscribe to that flow.

### Workflow 2 — receive a stream and play it

```powershell
# Play the incoming stream to the default device, bit-transparent
.\build\openaes67.exe recv --iface 192.168.1.10 --group 239.69.1.1 --port 5004 `
    --channels 8 --rate 48000 --exclusive --seconds 60
```

### Workflow 3 — use it as a 64×64 sound card (the DAW case)

```powershell
# 1. one process bridges both directions through the shared ring
.\build\openaes67.exe bridge --iface 192.168.1.10 `
    --group 239.69.1.1 --port 5004 `
    --channels 64 --ptime 1 --shm OpenAES67Audio `
    --send-group 239.69.1.2 --send-port 5006 --sap

# 2. in the DAW: select "OpenAES67 ASIO 64x64", 48 kHz
#    inputs 1-64  = what arrived from the network
#    outputs 1-64 = what is sent to the network
```

The bridge can also find the downlink by itself instead of taking an address:

```powershell
.\build\openaes67.exe bridge --iface 192.168.1.10 --discover --discover-name "Dante" `
    --channels 64 --shm OpenAES67Audio
```

If the Dante AES67 flow is not announced over SAP, copy its multicast address from
Dante Controller (Device View → Transmit) and use it directly; `probe` will tell you
the format if you do not know it:

```powershell
.\build\openaes67.exe probe --iface 192.168.1.10 --group 239.69.1.1 --port 5004
```

### Workflow 4 — GUI

```powershell
.\build-qt\Release\openaes67_gui.exe
```

- **开始** starts both directions at once: receive (network → 输出通道) and send
  (输入通道 → network)
- **声卡模式（ASIO 64×64）** replaces that pair with the ASIO bridge, for use with a DAW
- **环境诊断** runs the same checks as `doctor`, off the UI thread
- **注册 ASIO 驱动** requests elevation and registers the sound card
- **按地址接收** takes a `group:port` pasted from Dante Controller, probes the format
  and starts
- **开机自动启动** writes the Run entry described in section 3
- closing the window hides it to the tray; audio keeps running. **退出** in the tray
  menu is what actually stops it

### Config file

Every command accepts `--config FILE`; flags override the file:

```ini
iface = 192.168.1.10
group = 239.69.1.1
port  = 5004
channels = 64          # output channels (receive)
ptime = 1
codec = l16
shm   = OpenAES67Audio
send_group = 239.69.1.2
```

An unknown key is a startup error (exit code 2) rather than a silent no-op.
[`openaes67.example.conf`](openaes67.example.conf) is a commented starting point.

---

## 5. Input and output examples

All output below is from the commands as written, on a Windows 10 machine with a
USB Ethernet adapter.

### `doctor` — is this machine ready?

```
$ openaes67 doctor --iface 192.168.1.10 --group 239.69.1.1 --port 5004
OpenAES67 doctor
  [ OK ] interface: 192.168.1.10
  [ OK ] group: 239.69.1.1:5004 is multicast
  [....] sap: listening for announcements
  [WARN] sap: none received -- no AES67 sender on this network, or UDP 9875 is blocked
  [ OK ] asio: OpenAES67 ASIO 64x64 registered (asio_driver\out\openaes67_asio64.dll)
  [WARN] ptp: no grandmaster within 3 s -- Dante will not send audio until the clock locks

READY: no blocking problems found
```

Exit code 0 = ready, 1 = something blocking. `[WARN]` does not fail the check: no
sender on the network, or no grandmaster yet, is not a misconfiguration. On a machine
where the sound card has not been registered the ASIO line reads `[FAIL] ... run
asio_driver\register-asio-driver.bat as administrator` and the summary becomes
`NOT READY`.

### `sdp` — what would be announced

```
$ openaes67 sdp --iface 192.168.1.10 --group 239.69.1.2 --port 5006 --channels 4
v=0
o=- 0 1 IN IP4 192.168.1.10
s=OpenAES67 stream
t=0 0
a=sendonly
m=audio 5006 RTP/AVP 96
c=IN IP4 239.69.1.2/32
a=source-filter: incl IN IP4 239.69.1.2 192.168.1.10
a=rtpmap:96 L16/48000/4
a=ptime:1
a=ts-refclk:localmac=00-1D-C1-FF-FE-12-34-56
a=mediaclk:direct=0
a=sync-time:0
a=framecount:48
a=mid:1
```

### `probe` — unknown address, known format afterwards

```
$ openaes67 probe --iface 192.168.1.10 --group 239.69.6.6 --port 5066 --seconds 2
probing 239.69.6.6:5066 for 2.0 s
  packets    : 303
  format     : L24 4 ch @ 48000 Hz
  ptime      : 1.000 ms (48 frames/packet)
  payload    : type 97, ssrc 0xCAAD2F1C

recv command for this stream:
  openaes67 recv --iface 192.168.1.10 --group 239.69.6.6 --port 5066 --channels 4 --ptime 1.000 --codec L24
```

The format is derived from the RTP timestamps, so it works with no SDP at all.

### `tx` / `rx` — no audio hardware involved

```
$ openaes67 tx --iface 192.168.1.10 --group 239.69.1.1 --port 5004 --seconds 2
OpenAES67 tx
  iface   : 192.168.1.10
  dst     : 239.69.1.1:5004
  format  : L16 2 ch @ 48000 Hz, 48 frames/packet
  ttl     : 32
  ssrc    : 0x8A3563C7 (random)
  source  : 1000 Hz tone, amplitude 20000
  packets : 2000
done: 2000 packets in 2.00 s (999.0 pkt/s)
```

```
$ openaes67 rx --iface 192.168.1.10 --group 239.69.1.1 --port 5004 --seconds 3 --out capture.wav
OpenAES67 rx
  iface   : 192.168.1.10
  group   : 239.69.1.1:5004
  out     : capture.wav

result
  packets    : 3002
  other ssrc : 0 (from another source, ignored)
  ssrc change: 0 (sender restarts adopted)
  frames     : 144096 (3.002 s of audio)
  lost       : 0
  duplicates : 0
  late       : 0
  jitter buf : 0 packets remaining
  wav        : capture.wav
```

### `recv` — network to a device

```
$ openaes67 recv --iface 192.168.1.10 --group 239.69.1.1 --port 5004 --channels 64 --seconds 5
group   : 239.69.1.1:5004
sink: wasapi-shared, 64 ch @ 48000 Hz, packet=48 frames
prebuffer: reached 1440 of 1440 frames
packets    : 5086
frames out : 242688 (5.056 s)
lost       : 0
buffer     : mean=1435 target=1440 frames
```

### `bridge` — sound-card mode

```
$ openaes67 bridge --iface 192.168.1.10 --group 239.69.1.1 --port 5004 --channels 64 `
    --ptime 1 --shm OpenAES67Audio --send-group 239.69.1.2 --send-port 5006 --sap
OpenAES67 bridge (AES67 <-> shared ring <-> ASIO)
  section : OpenAES67Audio, 64 ch @ 48000 Hz
  downlink: 239.69.1.1:5004 -> ring
  uplink  : ring -> 239.69.1.2:5006
  asio    : OpenAES67 ASIO 64x64 registered (asio_driver\out\openaes67_asio64.dll)
            (or: "not registered -- run asio_driver\register-asio-driver.bat")
[rx] shm: section ready
[rx] ptp: slave on domain 0, waiting for a grandmaster
[sap] announcing uplink 239.69.1.2:5006
[rx] [  5.0 s] mean=  1441 corr=   +0.0 ptp=   +0.0 written=47998 Hz played=0 Hz devpad=-1
done: 2000 uplink packets (2000 without ASIO audio)
```

`without ASIO audio` is expected when no DAW has the sound card open: the ring is
empty, so silence is transmitted. It goes to zero once the DAW starts.

### `bench` — the numbers the optimisations are judged by

```
$ openaes67 bench
case                        value unit
ring 2ch                11263.640 Mframes/s
ring 64ch                 330.989 Mframes/s
queue 2ch                2225.622 Mframes/s
queue 64ch                677.813 Mframes/s
convert 64->2             609.861 Mframes/s
convert 64->64            447.337 Mframes/s
rtp pack+parse             30.260 Mops/s
sdp build                   4.039 us
```

Comparing against a recorded baseline (fails on more than 10 % regression):

```
$ openaes67 bench --baseline bench-baseline.txt
ring 64ch                 331.02 Mframes/s   was   330.99     0.0%  ok
...
BASELINE OK
```

### GUI log

The same events appear in the GUI, and the binary can be driven from a script:

```
$ openaes67_gui.exe --autostart both --autostop 8
[gui] 接收：L16 2 通道 239.69.8.8:5110
[gui] 发送：L16 2 通道 239.69.1.2:5006
[gui] sink: wasapi-shared, 2 ch @ 48000 Hz, packet=48 frames
[gui] 已发送 1000 包 | 有声音的节拍 0 | 静音填充 1000
```

---

## What Dante Controller shows (and what it never will)

Dante Controller lists two kinds of things:

1. **Dante devices** — discovered with Audinate's own protocol (mDNS-based).
2. **Third-party AES67 / ST 2110-30 transmit flows** — learned from SAP/SDP, shown
   in blue at the far right of the Routing tab and in the Receive tab's channel list.

A non-Dante AES67 **receiver** is not in either list, and cannot be: AES67 has no
"register as a receiver" step, and Dante Controller has no concept of one. Audinate
documents the same behaviour for every third-party device: only RTP *transmit* flows
appear
([Dante Controller user guide](https://dev.audinate.com/GA/dante-controller/userguide/webhelp/content/creating_aes67_or_st2110-30_rtp_transmit_flows.htm)).

So the two directions are set up on opposite ends:

| Direction | Who creates it | Where you see it |
|---|---|---|
| OpenAES67 → Dante | OpenAES67 announces the stream over SAP (`tx --sap`, `bridge --sap`) | Dante Controller: blue AES67 flow, right of the Routing matrix. Subscribe a Dante receiver to it. |
| Dante → OpenAES67 | The **Dante device** must create an AES67 multicast transmit flow (Device View → Transmit → Add multicast flow → AES67) | Dante Controller: that flow under the Dante device. OpenAES67 then listens to its multicast address — via SAP discovery, `--group/--port`, or `probe`. |

`bridge --sap` announces the **uplink** (what a DAW plays into the ASIO outputs), so
the return path is visible in Dante Controller as well. The downlink is a
subscription and is deliberately not announced: announcing a stream you only receive
would tell the network you are a sender.

The full derivation, including why faking a Dante device would be worse than being
invisible, is in [`docs/DANTE-VISIBILITY.md`](docs/DANTE-VISIBILITY.md).

---

## Layout

```
include/openaes67/   public headers (rtp, sdp, sap, jitter, net, engine, ptp, ...)
src/                 implementations, plus receive_loop.cpp shared by CLI and GUI
src/gui/             Qt 6 Widgets front end
asio_driver/         64-in/64-out ASIO sound card (user-mode COM DLL)
driver/              PortCls virtual audio driver (WDK; needs test signing)
tests/selftest.cpp   302 assertions, no external framework
tools/               Python transmitter/receiver/grandmaster, soak and perf scripts
assets/              icon set generated from logo.png
docs/FINDINGS.md     every measurement, trap and dead end, in order
docs/DANTE-VISIBILITY.md  why a Dante Controller cannot list a receiver
docs/M6-DESIGN.md    kernel <-> user shared-memory contract
bench-baseline.txt   recorded benchmark values for tools/verify-perf.ps1
```

The receive path lives in `src/receive_loop.cpp` and is called by both the CLI and
the GUI. That is deliberate: when the two had their own copies they drifted apart,
and it took a full day to notice.

---

## Honest limitations

- **The WDM driver cannot be loaded with Secure Boot on.** The ASIO sound card has
  no such requirement (it is a user-mode DLL), which is why it is the supported path.
- **Shared-mode capture is not bit-transparent.** The Intel SST speaker protection
  chain applies gain and compression before WASAPI loopback sees the data (measured:
  ratio 3.376 vs 2.039 across amplitudes). Bit-exact capture needs ASIO or our own
  driver. Playback in exclusive mode is bit-transparent.
- **PTP uses software timestamps.** There is a constant ~0.17 ms asymmetry bias, and
  the closed-loop frequency estimate is 11 % off because timestamps are taken in user
  space. Both need hardware timestamping, which USB adapters like the ASIX AX88179 do
  not provide.
- **64 channels at 1 ms exceeds a 1500-byte MTU** (6144 bytes per packet, so IP
  fragments). This works on a local NIC and on networks that carry it; a switch that
  drops fragments will not. Use 5 ms (30720 bytes, still fragmented) only if the
  whole path supports it, or fewer channels.
- **AES67 receivers do not appear in Dante Controller.** See the section above; this
  is a protocol boundary, not a missing feature.

---

## Roadmap

| # | Item | Status |
|---|---|---|
| M1 | Protocol core: RTP + SDP + self-test | ✅ |
| M2 | Send/receive engine: sockets, multicast, jitter buffer | ✅ |
| M3 | WAV source/sink + WASAPI capture/render | ✅ |
| M4 | WASAPI exclusive, PTPv2 slave, rate controller, closed loop | ✅ |
| M5 | Qt 6 GUI | ✅ |
| M6 | Kernel driver: ring ABI, PortCls adapter, WaveRT stream, signed package | ⏳ blocked on Secure Boot |
| M7 | Release engineering: CI, packaging | ✅ |
| M8 | **64 × 64 ASIO virtual sound card** | ✅ |
| M9 | Tooling: `doctor`, `probe`, `bench` + baseline gate, config file | ✅ |

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Two rules matter more than the rest:
behaviour must be traceable to a public standard, and numbers must come from a
reproducible measurement.

Before sending a change that touches a hot path, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\verify-perf.ps1
```

It takes the best of three runs and fails if any case is more than 10 % slower than
`bench-baseline.txt`. `set OPENAES67_PERF_GATE=1` makes the build run it
automatically (off by default: a timing gate on a busy machine fails for everyone).

## Package feed

Every release is also published as a NuGet package on GitHub Packages, which is
handy for CI or for pinning a version:

```bat
dotnet nuget add source https://nuget.pkg.github.com/ylzwd123/index.json ^
    -n openaes67 -u <github-user> -p <PAT-with-read:packages>
nuget install OpenAES67 -Source openaes67
```

It contains `tools\openaes67.exe`, the self-test and the ASIO sound card under
`tools\asio\`. Two things are worth knowing before you wire it into a pipeline:
GitHub Packages requires authentication even for public packages, and this is a
distribution package, not a library — `dotnet add package` gives you nothing to
call. The release archives remain the recommended path.

## Security

OpenAES67 opens sockets and parses untrusted network input. If you find a
problem in the RTP/SDP/SAP parsers, the WAV reader, the ASIO driver or the
registration scripts, please report it privately — see
[`SECURITY.md`](SECURITY.md). Do not open a public issue for it.

## License

MIT — see [`LICENSE`](LICENSE).

---

## CLI reference

`openaes67 help` prints the same list. Defaults are shown where they apply.

**Stream definition** (all commands)

| Option | Default | Meaning |
|---|---|---|
| `--iface IP` | first non-loopback | local IPv4 of the NIC used for multicast |
| `--group IP` | `239.69.1.1` | multicast destination (must be 224.0.0.0/4) |
| `--port N` | `5004` | destination UDP port |
| `--channels N` | `2` | channel count (1–64) |
| `--rate HZ` | `48000` | sample rate |
| `--ptime MS` | `1.0` | packet time in ms (48 frames at 48 kHz) |
| `--codec NAME` | `l16` | `l16` (2 bytes/sample) or `l24` (3 bytes/sample); L24 uses PT 97 |
| `--ttl N` | `32` | IP multicast TTL |
| `--ssrc N` | random | RTP SSRC; a random one is generated when omitted |
| `--config FILE` | — | read defaults from a `key = value` file; flags still win |

**`tx`** (transmit a tone or a WAV file)

| Option | Default | Meaning |
|---|---|---|
| `--seconds S` | `6.0` | duration; ignored when `--wav` is given (the whole file is sent) |
| `--freq HZ` | `1000` | test tone frequency |
| `--amplitude N` | `20000` | test tone amplitude (16-bit scale) |
| `--wav FILE` | — | send a WAV file instead of a tone (channels/rate come from the file) |
| `--ptp` | off | derive RTP timestamps from PTP; waits up to 5 s for a lock |
| `--domain N` | `0` | PTP domain used with `--ptp` |
| `--pace-ppm N` | `0` | run the sender fast (+) or slow (−) |
| `--rtcp` | off | send RTCP Sender Reports every 5 s to port+1 |
| `--sap` | off | announce over SAP |
| `--sap-interval N` | `30` | SAP base interval in seconds (±1/3 jitter) |
| `--print-sdp` | off | print the SDP before sending |
| `--sdp-out FILE` | — | write the SDP to a file |
| `--refclk ptp:GMID[:domain]` | local MAC | override the advertised reference clock |

**`send`** (capture the PC output and transmit it)

Takes the stream-definition options above plus:

| Option | Default | Meaning |
|---|---|---|
| `--in-device NAME` | default loopback | capture device (substring or index) |
| `--ptp-lock` | off | derive RTP timestamps from PTP |

It has no `--wav`: use `tx --wav FILE` to transmit a file.

**`recv` / `rx` / `bridge`**

| Option | Default | Meaning |
|---|---|---|
| `--device NAME\|INDEX` | system default | output device (substring or index from `list-devices`) |
| `--exclusive` | off | WASAPI exclusive mode (bypasses the audio engine) |
| `--backend NAME` | `wasapi` | `wasapi`, `asio` or `shm` |
| `--asio-driver NAME` | — | ASIO driver (requires an ASIO SDK build) |
| `--shm NAME` | — | `recv`: feed this section instead of a device; `send`: read it instead of loopback |
| `--send-group G` | recv group | `bridge`: uplink multicast group (the ASIO outputs go here) |
| `--send-port P` | recv port+2 | `bridge`: uplink port |
| `--discover` | off | `bridge`: find the downlink stream over SAP instead of using `--group/--port` |
| `--discover-name S` | — | `bridge`: only accept announcements whose `s=` contains S |
| `--discover-seconds N` | `4` | `bridge`: how long to listen before giving up |
| `--ptp` | off | feed the PTP rate estimate to the rate controller |
| `--domain N` | `0` | PTP domain used with `--ptp` |
| `--sdp-file FILE` | — | derive group, port, channels, rate and codec from an SDP (L16/L24) |
| `--out FILE` | `rx.wav` | WAV output for `rx` |
| `--out-bits N` | `16` | `rx` WAV depth: `16` or `24` |
| `--rtcp` | off | `recv`/`rx`: listen for Sender Reports on port+1 and log them |

**Other commands**

| Command | Meaning |
|---|---|
| `doctor` | environment check: interface, group, SAP receive, ASIO registration, PTP |
| `probe` | listen on an address and report the format it carries (channels, codec, ptime) |
| `info` | print the resolved stream parameters |
| `sdp` | print the SDP for a stream |
| `list-devices` | enumerate WASAPI and ASIO output devices |
| `sap-listen` | list the streams announced over SAP |
| `ptp` | measure offset and drift against a grandmaster |
| `bridge` | one process both ways: AES67 → shared ring → ASIO, ASIO → ring → AES67 |
| `bench` | measure the hot paths; `--baseline FILE --write-baseline` records them, `--baseline FILE` fails on a regression over 15 % |
| `capture-in` | record a real input endpoint to WAV (what a DAW would record) |
| `shm-read` | drain a shared section and report what arrived |
| `version` | print the version and exit |
| `help` | print the option summary |

**GUI** (`openaes67_gui.exe`)

| Option | Meaning |
|---|---|
| `--autostart send\|recv\|both\|bridge` | start immediately: `both` runs send and receive together; `bridge` = sound-card mode |
| `--diagnose` | run the environment check at startup (same checks as `openaes67 doctor`) |
| `--autostart-register` | write the HKCU Run entry so the GUI starts at login (uses the current mode) |
| `--autostart-unregister` | remove that Run entry |
| `--autostop S` | stop and quit after S seconds |
| `--in-device NAME` | preselect the capture device for send mode |
| `--out-device NAME` | preselect the output device for recv mode |

### Receive behaviour

- the playback device is opened at the **stream** rate; shared mode lets WASAPI
  convert, exclusive mode requires a match
- jitter buffer holds up to 200 packets; reordering tolerance is 4 packets, beyond
  that a packet counts as lost
- a single SSRC is latched: other sources on the same group are counted and ignored
- a sender restart (new SSRC) is adopted after 200 ms of silence and 25 consecutive
  packets from the new source
- oversized packets (> 2× the expected frames) are counted as `truncated` and dropped
- **silence is fed by wall clock when the network stops**, so the device buffer is
  never dragged dry and the listener hears clean silence instead of a click
- packets with an unexpected payload type are counted (`wrongPayloadType`) and skipped
  rather than decoded as the wrong format
