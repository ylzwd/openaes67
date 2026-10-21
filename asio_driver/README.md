# asio_driver — 64 × 64 ASIO virtual sound card

This directory is the user-mode ASIO **driver** (not the host side in `src/audio_io_asio.cpp`).

## Why ASIO and not WDM

| | WDM / PortCls (`driver/`) | ASIO (this directory) |
|---|---|---|
| Channel count | shared-mode WDM is practical to ~8, channel mask is 18 bits | 64 in / 64 out in one device, no OS mixing |
| Signing | kernel driver: needs test signing + Secure Boot off, or a real EV signature | **user-mode DLL: no kernel signing, no Secure Boot change** |
| Load | requires a reboot / driver install | copy DLL + registry entries, then restart the host app |
| Which apps | every Windows app | ASIO hosts (Audition, Reaper, Cubase, Nuendo, …) |

The project's WDM path stays for "every app at a few channels"; ASIO is how the
64 × 64 claim is actually delivered.

## Data path

```
Dante / AES67 network
   → openaes67 recv --shm <section>        (user-mode pump, already works)
   → shared ring, "render" buffer          (user mode writes, driver reads)
   → THIS ASIO DRIVER, input channels 0..63
   → Audition (record)

Audition (playback)
   → THIS ASIO DRIVER, output channels 0..63
   → shared ring, "capture" buffer         (driver writes, user mode reads)
   → openaes67 send --shm <section>        (not implemented yet)
   → AES67 network
```

No ABI change is needed: the existing contract already has "user writes / driver
reads" (render) and "driver writes / user reads" (capture). For ASIO the meaning
is inverted relative to WDM — here the *driver* is the consumer of network audio
— but the mechanics and offsets are identical, which is why the ring is reused
as-is.

## Prerequisite: the ASIO SDK

Steinberg's SDK is not redistributable, so it is not in this repository. Copy
three files from the SDK's `common/` directory into `third_party/asio/`:

```
third_party/asio/iasiodrv.h
third_party/asio/asio.h
third_party/asio/asiosys.h
```

Download: Steinberg developer portal → ASIO SDK (accept the licence).

## Build

```bat
asio_driver\build-asio-driver.bat
```

Without the header the script stops with a one-line explanation instead of
producing a DLL that silently lacks ASIO (same rule as `build.bat asio`).

## Register

An ASIO driver is a COM in-process server plus one registry key:

```
HKLM\SOFTWARE\ASIO\OpenAES67 ASIO 64x64
    CLSID    = {GUID of the class}
HKLM\SOFTWARE\Classes\CLSID\{GUID}\InprocServer32
    (Default) = path to openaes67_asio64.dll
    ThreadingModel = Apartment
```

`asio_driver\register-asio-driver.bat` writes both after checking the DLL exists;
`unregister-asio-driver.bat` removes them. Both need an elevated prompt.

## Status

- [x] Ring verified at 64 channels: `openaes67 recv --channels 64 --shm …` then
      `shm-read` reports `geometry: 64 ch @ 48000 Hz`, 48000 frames/s, peak 20000
- [ ] Driver DLL: **not written yet** — it cannot be compiled or tested until the
      SDK header is in place
- [ ] `send --shm`: the output direction does not exist yet (today `send`
      captures a render endpoint with WASAPI loopback)

## Honest limits

- ASIO hosts only. A non-ASIO application will not see this device; that app
  needs the WDM path (which is the 2-channel playback endpoint today).
- 64 channels of 48 kHz int16 is 6.1 MB/s per direction; the ring is sized for
  it (48000 frames capacity), but the host buffer must be at least 64 samples per
  channel or `bufferSwitch` overhead dominates.
- ASIO sample type is int32 LSB-aligned by convention here (`ASIOSTInt32LSB`),
  converted to int16 for the ring. The ring carries int16; up-converting keeps
  the existing ABI instead of widening it for one backend.
