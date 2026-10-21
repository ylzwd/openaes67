# Dante Controller stability on this machine

Measured 2026-10-18. Everything below is backed by a command whose output is
recorded in `docs/FINDINGS.md` §41.

## The crash was not a networking problem

`Get-WinEvent` over 14 days found **11 DanteController crash events**, and every
single one blamed the same module:

```
Faulting application name: DanteController.exe, version: 4.2.3.1
Faulting module name: GooglePinyin2.ime, version: 2.7.25.128
Exception code: 0xc000041d
```

`0xc000041d` is `STATUS_FATAL_USER_CALLBACK_EXCEPTION` — an exception raised
inside a user-mode callback. An IME hooking a Qt application and faulting in a
callback produces exactly this signature.

The IME was registered system-wide:

```
HKLM\SYSTEM\CurrentControlSet\Control\Keyboard Layouts\E0200804
    IME File   = GOOGLEPINYIN2.IME
    Layout Text = 谷歌拼音输入法 2
```

...but it was **not** in the user's input-method list. `Get-WinUserLanguageList`
showed only Microsoft Pinyin. So this IME served no purpose on this machine; it
was still being injected into processes and killing one of them.

### Fix applied

```
C:\Windows\System32\GOOGLEPINYIN2.IME
  -> C:\Windows\System32\GOOGLEPINYIN2.IME.disabled
```

The registry still points at `GOOGLEPINYIN2.IME`, and that file no longer exists,
so the IME cannot be loaded.

**Undo** (elevated shell):

```powershell
Rename-Item 'C:\Windows\System32\GOOGLEPINYIN2.IME.disabled' 'GOOGLEPINYIN2.IME'
```

### Verification

| Check | Result |
|---|---|
| Registered IME file present | **False** (was 3984 KB) |
| Dante Controller restarted, ran 70 s | still running, 145.6 MB, no crash |
| New crash events in that window | 0 |
| Module-level proof (IME absent from DC's module list) | **not established**: `Modules` reported only 5 entries even when elevated, which is implausible for a Qt app, so the enumeration is unreliable here |

Being explicit: the file is gone, which is sufficient to stop the injection, but I
could not show the module list directly.

## Firewall rules

There were **no** rules for Dante traffic. Added (inbound, allow, all profiles):

```
OpenAES67 Dante Discovery UDP  UDP  5353, 4440-4444, 8700-8705, 8708, 8800, 9875
OpenAES67 Dante Discovery TCP  TCP  4440-4444, 8700-8705, 8708
```

Note for future inspection: a **non-elevated** `Get-NetFirewallRule` on this
machine does not list these rules, so "no rules found" from a normal shell is a
false negative. Query them from an elevated shell.

**Undo**:

```powershell
Get-NetFirewallRule | Where-Object DisplayName -like 'OpenAES67 Dante*' | Remove-NetFirewallRule
```

## NIC power saving

The Dante interface is a **USB** adapter:

```
以太网 5  ASIX USB to Gigabit Ethernet Family Adapter #2   1 Gbps
```

| Property | Value before | Value now |
|---|---|---|
| WOL Link Power Saving | 启用 | **禁用** |
| Energy-Efficient Ethernet | 禁用 | unchanged |
| Green Ethernet | 禁用 | unchanged |
| Interrupt Moderation Rate | 禁用 | unchanged |
| JumboPacket | 禁用 | unchanged |
| AllowComputerToTurnOffDevice | Unsupported | (the driver does not expose it) |

Changing an advanced property **resets a USB NIC**: the link dropped to
`Disconnected` and came back by itself within 5 s, with `192.168.1.10/16`
restored. Do not do this while a download or a stream is running.

**Undo**:

```powershell
Set-NetAdapterAdvancedProperty -Name '以太网 5' -DisplayName 'WOL Link Power Saving' -DisplayValue '启用'
```

## Remaining risk factors (not changed)

1. **Three adapters, two of them disconnected with APIPA addresses**
   (`169.254.x`). Dante Controller has to choose an interface; disconnected
   adapters make that choice unreliable. If discovery stays flaky, pin the
   interface in *Dante Controller → File → Configure Dante Interfaces* or disable
   the unused adapters.
2. **`/16` subnet** on the Dante NIC (`192.168.1.10/16`) puts the broadcast
   domain far wider than a Dante network normally needs; a `/24` is the common
   practice.
3. **Dante Controller 4.2.3.1** was still installed at the time of writing; the
   current release is 4.18.0.11.

## Re-running the check

```powershell
# report only
.\tools\dante-stability.ps1

# apply what is safe to apply (needs an elevated shell)
.\tools\dante-stability.ps1 -Apply
```

Each change prints its own undo command. `-SkipIme`, `-SkipFirewall` and
`-SkipNic` let you apply a subset.

---

## After the upgrade to 4.18.1.1

Dante Controller was upgraded from 4.2.3.1 to **4.18.1.1** (installer 4.18.1.2) on
2026-10-18. The old `Program Files (x86)` install is gone; the new one lives in
`C:\Program Files\Audinate\Dante Controller`.

### Crash timeline

| Time | DC version | Faulting module |
|---|---|---|
| 21:43:47 | 4.2.3.1 | GooglePinyin2.ime |
| 21:44:04 | 4.2.3.1 | GooglePinyin2.ime |
| 21:55:20 | 4.2.3.1 | GooglePinyin2.ime |
| *IME file renamed* | | |
| 22:24 onward | **4.18.1.1** | **no crashes** |

Every crash predates the IME rename and belongs to the old version.

### Runtime observation of 4.18.1.1

| Metric | t=0 | t+1 min | t+2 min | t+3 min | t+4 min |
|---|---|---|---|---|---|
| Working set | 301.0 MB | 310.9 MB | 349.2 MB | 353.6 MB | 354.5 MB |
| **Private bytes** | **484.3 MB** | 489.1 MB | 477.6 MB | 487.0 MB | 488.9 MB |
| Threads | 93 | 101 | 94 | 96 | 103 |
| Handles | 1259 | 1355 | 1292 | 1327 | 1383 |

Private bytes are **flat** (484 → 489 MB) while the working set rises ~18% and then
settles: that is the process touching pages it already owns, not a leak. Thread
count oscillates around 95. Handles drift +124 over 5 minutes; worth watching but
small.

Process layout of 4.18: a 6.6 MB launcher plus a ~300 MB main process (92 threads).

### The new version brought its own firewall rules

The 4.18 installer registered its own inbound rules, including one literally named
**"Dante Controller AES67"**, alongside `Dante Controller UDP/TCP`,
`Dante Controller Agent`, `Dante Controller Gateway`, `Dante Controller Conmon`
and `Dante Activator`. That is independent evidence that this version has AES67
support.

### Interface binding was already correct

`文件 → 接口` shows:

```
主接口   以太网 5      IP 192.168.1.10   MAC 00:1D:C1:FF:FE:12:34:56
备用接口 无
```

So the "Dante Controller is listening on the wrong NIC" hypothesis is **wrong** for
this machine. Yet `组播音频带宽` still reads `0 bps` and the device count is 0.