# driver — M6 toolchain probe

This directory exists to prove that this machine can build a Windows kernel
driver, and to hold the build script the real audio driver will use.

**`openaes67_probe` is not the audio driver.** It creates one device object and a
symbolic link, then does nothing. Its only job is to exercise the four steps a
driver needs:

1. compile against the WDK kernel headers
2. link against the WDM libraries (`ntoskrnl`, `hal`, `wdm`)
3. stamp and validate the INF (`stampinf`, `Inf2Cat`)
4. sign the package (`signtool`, locally generated test certificate)

All four pass. See [`../docs/M6-FEASIBILITY.md`](../docs/M6-FEASIBILITY.md) for
what remains before an actual audio miniport.

## Build

```powershell
.\driver\build-driver.bat
```

Two targets are built:

| Target | What it is |
|---|---|
| `openaes67_probe.sys` | toolchain probe: one device object, nothing else |
| `openaes67_audio.sys` | PortCls adapter + topology miniport + WaveRT render miniport |

Output lands in `driver\out\`:

```
openaes67_probe.sys    5,008 B   signed kernel driver (probe)
openaes67_audio.sys   11,264 B   PortCls adapter + topology + WaveRT render stream
openaes67_probe.cat    2,456 B   signed catalog produced by Inf2Cat
openaes67_probe.inf    1,206 B   stamped INF (probe device)
openaes67-test.cer       790 B   the self-signed test certificate
```

`openaes67_audio.sys` imports `portcls.sys` and `ntoskrnl.exe` and is linked as
`SUBSYSTEM:NATIVE` — i.e. it really is a PortCls miniport driver, not just a
kernel binary that happens to link.

`openaes67_audio.sys` registers **two subdevices** (`Topology` and `Wave`) and
implements the WaveRT stream: `AllocateAudioBuffer` hands PortCls a 10 ms cyclic
buffer, `GetPosition` derives the play position from the system clock, and a
1 ms timer DPC refills the buffer from the shared ring.

Both remaining code pieces are now in place:

1. **The ring is mapped from the kernel.** `NewStream` calls
   `CMiniportWaveRTStream::OpenRing(L"\\BaseNamedObjects\\OpenAES67Audio")`,
   which does `ZwOpenSection` + `ZwMapViewOfSection` and validates the header
   (magic, version, geometry). Missing section is not fatal: the stream then
   plays silence, which is the correct behaviour when the pump is not running.
2. **The audio INF exists** (`openaes67_audio.inf`): MEDIA class, one render
   endpoint, registered under `KSCATEGORY_AUDIO` and `KSCATEGORY_RENDER`.
   Inf2Cat reports `Signability test complete. Errors: None`.

So the driver is **code complete** and produces a signed package:

```
driver\out\audio\openaes67_audio.sys   13,712 B   signed
driver\out\audio\openaes67_audio.cat    2,456 B   signed
driver\out\audio\openaes67_audio.inf    1,969 B   stamped
```

**What is still missing is not code**: loading a test-signed kernel driver needs
Secure Boot off on this machine. See "Loading it" above.

Build requirements, all present on this machine:

- Windows SDK 10.0.26100
- WDK 10.0.26100 (only the headers and the `km` libraries are needed; see below)
- Visual Studio 2022 Build Tools with the C++ toolset

## Loading it (not done, and not doable unattended)

`BUILD OK` does **not** mean the driver will load. Two things are required, and
both need a human:

1. **Secure Boot off, then test signing on.** This machine currently reports
   `UEFISecureBootEnabled = 1`, and while Secure Boot is on, `testsigning` is
   ignored: the driver still will not load. The order matters.

   ```
   # 1. UEFI setup: disable Secure Boot, save, boot Windows
   # 2. elevated prompt, then reboot:
   bcdedit /set testsigning on
   ```

   Windows shows a "Test Mode" watermark on the desktop when it took effect.
   Turning it back off (`bcdedit /set testsigning off`, then reboot) removes it.

2. **The test certificate trusted.** `install-test-driver.ps1` does this, but it
   must run elevated:

   ```powershell
   powershell -ExecutionPolicy Bypass -File driver\install-test-driver.ps1
   ```

   It checks both preconditions before touching anything, imports
   `openaes67-test.cer` into `LocalMachine\Root` and
   `LocalMachine\TrustedPublisher`, verifies the catalog signature, installs the
   package with `pnputil`, and prints which device appeared. If a precondition is
   missing it stops with the exact command to fix it and changes nothing.

   To undo everything:

   ```powershell
   powershell -ExecutionPolicy Bypass -File driver\uninstall-test-driver.ps1
   ```

   That removes the device node, the driver package and both certificate
   entries. It deliberately does **not** change boot configuration: turning test
   signing off and re-enabling Secure Boot stay manual, because they are boot
   security decisions.

### What the installed package gives you today

A **playback** endpoint only. The INF registers `KSCATEGORY_AUDIO` and
`KSCATEGORY_RENDER`; there is no `KSCATEGORY_CAPTURE`, so recording applications
(Audition, a DAW) cannot see it as an input. Audio also arrives at 2 channels /
48 kHz, because the WaveRT stream advertises exactly that
(`nChannels = 2`, `kFrameBytes = 4` in `openaes67_audio.cpp`).

So "install it and record in Audition" needs one more milestone: a capture
miniport plus `KSCATEGORY_CAPTURE` in the INF, and a channel count that follows
the shared ring instead of the hard-coded stereo.

### 中文摘要

- 先关 Secure Boot（UEFI），再 `bcdedit /set testsigning on`，然后重启。
- 提权运行 `driver\install-test-driver.ps1`：它会先检查上面两个前提，
  再导入证书、验证签名、安装驱动，并打印出现的设备。
- 卸载用 `driver\uninstall-test-driver.ps1`；关闭测试签名仍需手动。
- **当前包只有播放端点（2 声道）**，Audition 需要录音端点，那是下一个里程碑。


## Notes for the real driver

The WDK on this machine installed **only the SDK half**:

| Component | Present | Consequence |
|---|---|---|
| `Include\10.0.26100.0\km` | ✅ 229 files | kernel headers available |
| `Lib\10.0.26100.0\km\x64` | ✅ 82 libs incl. `portcls.lib`, `ks.lib` | PortCls miniport can link |
| KMDF runtime libraries | ❌ | no WDF-based driver; use WDM |
| WDK MSBuild integration | ❌ | no `.vcxproj` driver targets; use `cl`/`link` directly |

That is why this build script calls `cl` and `link` itself instead of MSBuild:
the PortCls audio miniport we want is a classic WDM driver, so KMDF is not
required. If KMDF is ever wanted, add the WDK Visual Studio component and switch
to MSBuild.

## Traps already hit (so they are not hit twice)

| Symptom | Cause | Fix |
|---|---|---|
| `ntdef.h(197): #error "No Target Architecture"` | `/DAMD64` alone is not enough | add `/D_AMD64_` |
| `Inf2Cat: postdated DriverVer not allowed` | `stampinf -d *` writes the **local** date; Inf2Cat compares against **UTC** | stamp `(Get-Date).AddDays(-1)` |
| `22.9.10: missing from [SourceDisksFiles]` | the INF listed the binary in `CopyFiles` only | add `[SourceDisksFiles]` and `[SourceDisksNames]` |
| build hangs on `Enter password:` | `Get-PfxCertificate` prompts on stdin | export the `.cer` when the certificate is created |
| build hangs on a Windows security dialog | importing into the Root store prompts | never import automatically |
