# Audio devices

Up to 0.5.0 every backend silently used the system default endpoint, which made
a machine with an onboard codec, a virtual cable and a USB interface impossible
to drive deliberately. There is now an explicit device choice in both the CLI and
the GUI.

---

## Listing what the machine offers

```powershell
.\build\openaes67.exe list-devices
```

Measured on this machine (2026-10-18):

```
OpenAES67 audio outputs

  [ 0] wasapi  扬声器 (Realtek(R) Audio)   (default)
  [ 1] wasapi  扬声器 (ToDesk Virtual Audio)
  [ 2] wasapi  扬声器 (网易虚拟音频设备)
  [ 3] asio    Digisyn-vSndCard
  [ 4] asio    DSD 转码器 (DoP/Native)
  [ 5] asio    Madis UT44

ASIO backend: not built
```

WASAPI endpoints come from `IMMDeviceEnumerator::EnumAudioEndpoints`. ASIO drivers
come from the registry (`HKLM\SOFTWARE\ASIO`, both the 64-bit and 32-bit views),
which is why they appear even in builds that cannot open them yet: hiding a
detected interface would leave the user guessing.

---

## Choosing a device

```powershell
# by index from list-devices
.\build\openaes67.exe recv --iface 192.168.1.10 --device 1

# by name substring
.\build\openaes67.exe recv --iface 192.168.1.10 --device "ToDesk"

# exclusive mode on that same endpoint
.\build\openaes67.exe recv --iface 192.168.1.10 --device "ToDesk" --exclusive
```

The log now prints what was actually opened:

```
sink: wasapi-shared, 2 ch @ 48000 Hz, packet=48 frames
device: 扬声器 (ToDesk Virtual Audio)
```

An unknown name fails loudly instead of falling back to the default:

```
no audio output matches 'NoSuchDevice' (try list-devices)
```

In the GUI the first field is **输出设备**. It lists the system default, then every
endpoint, then every ASIO driver, and logs a one-line summary at startup so an
empty list is never mistaken for a populated one:

```
输出设备：WASAPI 3 个，ASIO 3 个（ASIO 后端未编译，暂不能打开）
```

---

## ASIO

### Why the backend is not enabled by default

The ASIO interface and SDK belong to Steinberg. Their licence does not allow
redistributing the SDK, so this repository ships **no** ASIO header. Two
consequences, both deliberate:

- a clone builds without anyone having to accept a third-party licence;
- the ASIO stub still *lists* installed drivers (registry data only) and explains
  why it cannot open them.

### Turning it on

1. Download the ASIO SDK from Steinberg's developer page and accept their licence.
   The files needed are `iasiodrv.h`, `asio.h` and `asiosys.h` from `common/`.
2. Put them in `third_party\asio\`.
3. Rebuild:

   ```powershell
   .\build.bat asio                      # standalone build
   # or
   cmake -S . -B build -DOPENAES67_WITH_ASIO=ON
   ```

   Both refuse to build with a clear message if the header is missing, rather
   than silently producing a binary without ASIO.

Then the driver can be opened by its registry key:

```powershell
.\build\openaes67.exe recv --iface 192.168.1.10 --asio-driver "Madis UT44"
# --asio-driver implies --backend asio
```

### Status

| Part | State |
|---|---|
| Driver enumeration (registry) | ✅ verified, see the listing above |
| Device selection for WASAPI | ✅ verified, opens the chosen endpoint |
| ASIO sink implementation | written, **not compiled or tested here** |
| ASIO on real hardware | ⚠️ unverified — needs the SDK and the interface plugged in |

The ASIO sink code path is written against the documented `IASIO` interface
(`init` / `getBufferSize` / `createBuffers` / `setSampleRate` / `start`, with the
`bufferSwitch` callback staging samples). It has never been compiled on this
machine because the SDK is not present, so treat it as a starting point that needs
one session with the actual interface, not as tested code.

---

## Why ASIO matters here

The measured weakness of the WASAPI path is that shared-mode capture is not
bit-transparent: Intel Smart Sound Technology's speaker protection applies gain
and compression *before* the loopback tap (measured ratio 3.376 vs 2.039 against
the same source). Exclusive mode bypasses the audio engine and every APO, which
fixes playback, but capture has no exclusive equivalent.

ASIO drivers talk to the interface directly, so for a USB interface with a vendor
ASIO driver both directions can be bit-exact and the latency is set by the driver
rather than by the Windows audio engine. That is the reason this backend exists.
