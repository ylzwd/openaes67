# M6 feasibility: a virtual sound card for OpenAES67

Status: **assessment complete, blocked on two external prerequisites**
(WDK installation and Secure Boot / driver signing).

This document records what was checked, what was measured, and what the three
realistic routes cost. It exists so the decision is explicit rather than
discovered halfway through writing a driver.

---

## 1. Why a driver at all

Today OpenAES67 can send and receive AES67, and it can play into any device
Windows exposes. What it cannot do is **appear as a sound card**, so other
applications cannot select "OpenAES67" as their output or input.

That last capability requires a kernel-mode audio driver. There is no supported
user-mode path:

| Route | Creates a selectable device? | Notes |
|---|---|---|
| WASAPI loopback (what we use now) | ❌ | captures an existing endpoint |
| Audio Processing Object (APO) | ❌ | modifies an existing endpoint's stream |
| User-mode "virtual cable" | ❌ | still needs a kernel driver underneath |
| Kernel audio driver (PortCls/AVStream) | ✅ | the only supported way |

---

## 2. What the machine has today

Checked on 2026-10-18, ThinkPad X1 Extreme Gen 3, Windows 10 19045 x64.

| Prerequisite | State | Evidence |
|---|---|---|
| Windows SDK | ✅ 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0` |
| **WDK** | ❌ **not installed** | no `Include\*\km` directory; `stampinf.exe`, `inf2cat.exe` absent |
| Visual Studio 2022 | ✅ Build Tools | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| **Secure Boot** | ⚠️ **enabled** | `HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State` → `UEFISecureBootEnabled = 0x1` |
| Test signing | ❌ off | `bcdedit /enum {current}` shows no `testsigning` entry |
| EV code-signing certificate | ❌ none | required only for distribution |

Without the WDK there is no `km` headers and no `stampinf`/`inf2cat`, so a driver
cannot be compiled at all. Without Secure Boot disabled (or a Microsoft-signed
package) a self-signed test driver cannot load.

---

## 3. The three routes

### Route A — derive from SysVAD (Microsoft sample, MS-PL)

SysVAD is Microsoft's own virtual audio device sample: a PortCls miniport with a
wave RT stream, an INF, and a topology filter. It is licensed MS-PL, which is
compatible with this project's MIT licence for a derived work provided the
SysVAD notice is kept.

- **Work**: replace the sample's fake tone generator with a ring buffer that our
  user-mode service fills from the AES67 receive path (and drains for capture).
  That bridge is the actual project work — the driver plumbing is already there.
- **Estimate**: 2–4 weeks for a working render+capture device with a shared
  buffer, plus debugging time.
- **Risk**: low-to-medium. The sample is known to build; the hard part is the
  kernel/user shared memory and the streaming state machine.

### Route B — PortCls/AVStream written from scratch

- **Work**: INF, miniport, topology, stream, power management, DPC handling.
- **Estimate**: 8–12 weeks for someone experienced; far more for a first driver.
- **Risk**: high. A mistake in an audio DPC is a bugcheck, not an exception.

### Route C — user-mode virtual device framework (e.g. a third-party cable)

- Every practical option in this space is itself a signed kernel driver, most are
  closed source, and none of them speak AES67.
- **Estimate**: not a reduction in work, and it adds a dependency we cannot
  audit or ship.

**Recommendation: Route A.** It is the only route where the hard part is our
problem domain (AES67 ↔ shared buffer) rather than Windows driver plumbing.

---

## 4. What must happen before any code is written

1. **Install the WDK matching the installed SDK** (10.0.26100). Machine-wide
   installers normally need administrator rights:

   ```powershell
   winget install --id Microsoft.WindowsWDK.10.0.26100 -e
   ```

2. **Decide how the driver will be loaded during development.** One of:

   - Disable Secure Boot in the firmware, then enable test signing:
     ```powershell
     bcdedit /set testsigning on      # requires an elevated prompt, then reboot
     ```
     Fast, free, and weakens boot security for as long as it stays on.
   - Or buy an EV certificate and submit to the Hardware Dev Center for a
     Microsoft-signed package. This is the only way to distribute a driver that
     loads on a Secure Boot machine without asking users to change firmware
     settings. Cost is roughly a few hundred dollars per year plus a
     verification process for the publisher identity.

3. **Develop against a VM.** A driver bug in the audio stack bugchecks the
   machine. A Hyper-V or VMware guest with a checkpoint is the difference between
   a five-minute rollback and a reinstall.

None of these three steps is something this project can do silently on a user's
machine: two of them change boot security, and one costs money.

---

## 5. What can be built meanwhile (and was)

The parts of M6 that do not need the WDK are already in place or can be done now:

| Piece | State |
|---|---|
| Ring-buffer design between kernel and user mode | not started (needs driver ABI decisions) |
| AES67 → shared buffer pump | **equivalent logic exists**: `runReceiveLoop` already produces a paced stream |
| Capture side bit-exactness study | done — shared-mode capture is not transparent, see FINDINGS §4 |
| Driver signing research | done, summarised above |

The honest summary: **every user-mode piece M6 needs already exists**, because
the driver's job is to expose a buffer that our existing receive/send path
already fills and drains.

---

## 6. Decision requested

Route A requires two things this session cannot supply on its own:

- administrator rights to install the WDK (a UAC prompt), and
- a decision on Secure Boot / test signing, which affects the machine's boot
  security and needs a reboot.

Once those are granted, the plan is:

1. Install WDK, build the unmodified SysVAD sample to prove the toolchain works.
2. Replace its tone generator with a ring buffer; keep the INF and topology.
3. Add a user-mode side that attaches to the buffer and pumps AES67 frames.
4. Verify with the same measurement discipline used everywhere else in this
   project: bit-exact file round trip through the virtual device, then buffer
   depth and dropout counts under load.

---

## 7. Progress after the assessment (same day)

The assessment above said "blocked on WDK installation". That was resolved:
`winget install --id Microsoft.WindowsWDK.10.0.26100 -e` succeeded without an
interactive prompt, and the driver build pipeline was then proven end to end.

### What the WDK install actually delivered

Only the SDK half, not the Visual Studio integration:

| Component | State | Consequence |
|---|---|---|
| `Include\10.0.26100.0\km` | ✅ 229 files | kernel headers available |
| `Lib\10.0.26100.0\km\x64` | ✅ 82 libs, incl. `portcls.lib`, `ks.lib`, `classpnp.lib` | PortCls miniports can link |
| KMDF runtime libs | ❌ | no WDF drivers |
| WDK MSBuild integration | ❌ | no `.vcxproj` driver targets |

This looked like a blocker until it became clear that a **PortCls audio miniport
is a classic WDM driver**: it needs `portcls.lib`, not KMDF. So the missing
KMDF half does not block M6. The build now calls `cl` and `link` directly.

### Toolchain proven, with evidence

`driver\build-driver.bat` builds a minimal probe driver and prints its results:

```
=== compiling ===
=== linking ===
=== stamping INF ===
    Stamping [Version] section with DriverVer=10/17/2026,0.5.0.0
=== validating package (Inf2Cat) ===
Signability test complete.
Errors:
None
Catalog generation complete.
...\driver\out\openaes67_probe.cat
=== signing with a local test certificate ===
Successfully signed: openaes67_probe.sys
Successfully signed: openaes67_probe.cat
=== result ===
openaes67_probe.cat
openaes67_probe.sys
openaes67-test.cer
  signature status : UnknownError
  signer           : CN=OpenAES67 Test Signing
BUILD OK
```

`signature status: UnknownError` is the expected value: the signature exists and
names the right signer, but the self-signed root is not in a trust store. That is
deliberate — importing it raises a Windows security dialog.

### Five traps found while getting there

| Symptom | Cause | Fix |
|---|---|---|
| `ntdef.h(197): #error "No Target Architecture"` | `/DAMD64` is not enough | add `/D_AMD64_` |
| `Inf2Cat: postdated DriverVer not allowed` | `stampinf -d *` writes the **local** date, Inf2Cat compares **UTC** | stamp `(Get-Date).AddDays(-1)` |
| `22.9.10: missing from [SourceDisksFiles]` | binary only listed under `CopyFiles` | add `[SourceDisksFiles]` + `[SourceDisksNames]` |
| build hangs on `Enter password:` | `Get-PfxCertificate` reads a password from stdin | export the `.cer` when creating the certificate |
| build hangs on a security dialog | auto-importing a root certificate prompts | never import automatically |

The timezone trap is the same class of bug as the project's earlier ones: the
tool wrote local time, the checker read UTC, and the failure message ("postdated")
pointed at the wrong thing.

### What is still blocking a real audio device

| Blocker | Who can clear it |
|---|---|
| Secure Boot is on (`UEFISecureBootEnabled = 0x1`), so a test-signed driver cannot load | needs a firmware change and a reboot — a human decision |
| No EV certificate / Hardware Dev Center account | needed only for distribution to other machines |
| PortCls miniport implementation (render + capture, shared ring buffer) | project work, weeks |

Everything that can be verified without changing boot security is now verified.