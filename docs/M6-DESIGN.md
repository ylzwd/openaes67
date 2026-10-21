# M6 design: kernel ↔ user shared audio ring

This is the contract between the PortCls audio driver and the user-mode AES67
pump. It is implemented and tested today on the user-mode side; the driver side
is what a miniport will have to honour.

Implemented in [`include/openaes67/shared_audio.h`](../include/openaes67/shared_audio.h)
(ABI, C-compatible) and
[`src/shared_audio_ring.cpp`](../src/shared_audio_ring.cpp) (user-mode side).

---

## 1. Why a shared ring instead of IOCTLs per packet

An audio driver is called every few milliseconds and must never block. A ring in
shared memory means:

- the driver's stream callback does a `memcpy` and one index update
- no IOCTL on the audio path, so no scheduling jitter from user mode
- the user-mode pump can run at its own pace and simply starve (documented
  underrun) rather than stall the driver

IOCTLs are still used for control: starting/stopping the stream, querying
geometry, and resetting counters. They are never on the data path.

---

## 2. Memory layout

```
offset 0                      : OpenAes67SharedAudio header
offset sizeof(header)         : render ring  (capacityFrames * frameBytes)
offset sizeof(header) + size  : capture ring (capacityFrames * frameBytes)
```

Both rings are interleaved 16-bit little-endian PCM, `channels` samples per
frame. Total mapping size:

```c
openaes67_mapping_size(capacityFrames, frameBytes)
    = sizeof(OpenAes67SharedAudio) + 2 * capacityFrames * frameBytes
```

The helpers in the header are the only supported way to compute offsets; do not
hard-code them.

### Header fields

| Field | Written by | Meaning |
|---|---|---|
| `magic`, `version` | creator | `0x4F413637` ("OA67"), currently `1` |
| `frameBytes`, `channels`, `sampleRate`, `capacityFrames` | creator | immutable geometry |
| `flags` | driver | `RUNNING`, `UNDERRUN`, `OVERRUN` bits |
| `writeIndex` | user mode | render frames produced |
| `readIndex` | driver | render frames consumed |
| `captureWrite` | driver | capture frames produced |
| `captureRead` | user mode | capture frames consumed |
| `renderUnderruns`, `captureOverruns` | driver | diagnostics only |

Indices are **free-running 32-bit frame counters**, not array offsets. They wrap
at 2^32 and unsigned subtraction yields the correct distance across a wrap, which
removes the classic "full vs empty" ambiguity of `head == tail`. Use
`openaes67_frames_pending()` / `openaes67_frames_free()` rather than open-coding
the comparison.

---

## 3. Ordering rules

Both directions are single-producer/single-consumer, so no lock is required, but
the ordering is not optional:

**Producer**

1. `memcpy` the samples into the ring at `writeIndex % capacityFrames`
2. release barrier
3. publish the new `writeIndex`

**Consumer**

1. read `writeIndex` (acquire)
2. `memcpy` out of the ring at `readIndex % capacityFrames`
3. release barrier
4. publish the new `readIndex`

Skipping the barrier lets the CPU or compiler move the `memcpy` after the index
publish, and the other side reads stale samples. The user-mode side uses
`std::atomic_thread_fence`; the driver must use `KeMemoryBarrier()` (or an
interlocked exchange of the index) at the same points.

---

## 4. Behaviour under stress

Deliberately non-blocking, because blocking in either context is worse than a
glitch:

| Situation | Behaviour |
|---|---|
| Producer outruns consumer | write accepts only what fits; the excess is dropped and `OVER`/`OVERRUN` is recorded |
| Consumer outruns producer | read returns fewer frames than asked; the driver fills the remainder with silence and bumps `renderUnderruns` |

The self-test asserts both: a full ring accepts zero further frames, and an empty
ring returns zero rather than blocking.

---

## 5. Sizing

`capacityFrames` is chosen by the pump when it creates the section. The current
default is 500 ms at 48 kHz, which is far more than the 30 ms the rate controller
targets: the ring is a shock absorber for scheduling hiccups, not the jitter
buffer. The jitter buffer stays in user mode (`RateController`), where it can be
observed and tuned without touching the driver.

Latency added by the ring at steady state is one packet in flight, because the
driver drains whatever is pending at each callback.

---

## 6. What the miniport must implement

The driver side has three obligations:

1. **WaveRT render stream**: allocate a cyclic buffer, and on each callback pull
   from the render ring with `openaes67_frames_pending`; zero-fill on underrun.
2. **WaveRT capture stream**: push microphone/stream data into the capture ring;
   if it is full, drop the oldest frames and record `captureOverruns`.
3. **Control interface**: IOCTL to report geometry, set the `RUNNING` flag, and
   reset counters. Nothing that runs per period.

A miniport can be built without KMDF on this machine (the WDK here ships only the
WDM half), which is why the design avoids WDF APIs entirely.

---

## 7. Verification so far

`tests\selftest.cpp` covers the ABI logic without any driver:

| Test | Asserts |
|---|---|
| header validates | magic/version/size checks reject a bad mapping |
| geometry | channels, rate and capacity survive the round trip |
| wrap | 40 written, 24 drained, 40 more written: read returns 24..39 then 0..23, bit-exact |
| overrun | a full ring accepts zero further frames |
| underrun | an empty ring returns zero frames |
| capture | capture direction round-trips bit-exactly |
| named section | a real named section is created and attached twice |
| two views | data written through one view reads back bit-exactly through the other |

The last two exercise the actual Windows section object, not just the arithmetic.
They also caught a real bug: closing the section handle right after mapping kills
the name, so a second `OpenFileMapping` fails even though views still exist.

**Not verified yet**: the driver half. It cannot load on this machine while Secure
Boot is enabled (see [M6-FEASIBILITY.md](M6-FEASIBILITY.md)).
