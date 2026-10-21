# Contributing to OpenAES67

Thanks for your interest. This project is deliberately small and boring:
a standards-based AES67 implementation for Windows, MIT licensed, with no
accounts, no activation and no phone-home.

## Ground rules

1. **Standards first.** Behaviour must be traceable to AES67-2018, RFC 3550
   (RTP), RFC 4566 (SDP) or IEEE 1588 (PTP). If a change cannot be justified
   from a public standard, say so explicitly in the pull request.
2. **No vendor code, binaries or trademarks.** Do not paste code, DLLs, drivers,
   strings or product names from any commercial implementation. The project has
   zero occurrences of vendor identifiers by policy; CI enforces it.
3. **No licensing or activation logic.** The software must remain usable the
   moment it is built. Pull requests adding registration, serial checks, licence
   servers or telemetry will be rejected.
4. **Evidence over claims.** Numbers in `docs/FINDINGS.md` and `README.md` must
   come from a reproducible measurement. State the command, the hardware and the
   conditions. "Works on my machine" is not evidence.

## Building

```powershell
# Direct MSVC build (fastest loop, no CMake needed)
.\build.bat

# CMake build (this is what CI runs)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Self-test: 320 assertions covering RTP, SDP, SAP, PTP, jitter buffer and rate control
.\build\Release\openaes67_selftest.exe
```

Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload and the
Windows 10/11 SDK. The Qt GUI is optional and only built when CMake finds Qt 6.

## Before opening a pull request

- `openaes67_selftest.exe` prints `ALL PASS (0 failures)`
- both build paths succeed (`build.bat` and CMake)
- no new occurrence of vendor identifiers, licence checks or telemetry
- if you changed behaviour on the audio path, include a measurement
  (buffer depth, dropouts, latency) rather than a description

## Reporting bugs

Include:

- output of `openaes67.exe recv --iface <ip> --seconds 10` (or the relevant
  command)
- Windows version, audio device and network adapter
- whether the stream was exclusive or shared mode
- for timing issues: `devpad` and `played` columns from the periodic log

## Style

- C++17, no exceptions across the audio path, no allocation in the inner loop
- comments explain *why*, not *what*; the code already says what
- keep the receive path in `src/receive_loop.cpp` — the CLI and GUI both call it,
  and duplicating that logic is how the two paths drifted apart once already
## 性能门禁

改动热路径（收包循环、共享环、ASIO 泵）后跑一次：

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\verify-perf.ps1
```

它取 3 次运行的最好值，与 `bench-baseline.txt` 比较，超过 15% 退化即失败。
需要在新机器上重建基线时：

```bat
build\openaes67.exe bench --baseline bench-baseline.txt --write-baseline
```

构建时加 `set OPENAES67_PERF_GATE=1` 可让门禁在构建末尾自动跑（默认关闭：
计时的门禁在忙的机器上会误报）。

## 提交前自检

仓库里不能出现凭据、个人数据、本机绝对路径、构建产物或测试垃圾。构建会自动检查，
也可以单独跑：

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\scan-secrets.ps1
```

退出码 0 = 干净，1 = 有发现（按类别列出文件与行号）。

## 文档里不要复述敏感值

清理报告、提交信息、验证命令里都**不要写出被清理的具体值**。写"已替换为占位地址"就够了：

```
✅  git log --all -S "<真实网卡地址>"  -> 0 个提交
❌  git log --all -S "192.168.x.x"      -> 0 个提交
```

这条规则来自实际教训：同一类失误在五轮清理里出现了四次 —— 解释"我删掉了什么"时，
作者会本能举出具体值，于是把刚清理掉的东西重新写回文档。**机器能查的交给
`tools\scan-secrets.ps1`，人工只写结论。**

同理，示例里的地址统一用文档地址（`192.0.2.x` / `198.51.100.x`）或通用私有地址
（`192.168.1.x`），不要用本机实际值。

## 发版

一个版本号，一条命令：

```powershell
.\tools\release.ps1 -Version 0.2.0 -NotesFile notes.md
```

它按固定顺序做完整条链路：

```
改版本号（version.h / CMakeLists.txt / csproj）
  -> build.bat + ASIO 声卡
  -> 自检 + verify-claims + scan-secrets + verify-perf
  -> 提交并推送
  -> 等 CI 绿（红了就停，绝不给红提交打 tag）
  -> 下载 CI 产物、打两个压缩包、算 SHA256
  -> 打 tag 并推送
  -> 建 GitHub Release 并上传附件
  -> 推 NuGet 包
  -> 重新下载所有附件校验哈希与版本
```

先看它要做什么，不动任何东西：

```powershell
.\tools\release.ps1 -Version 0.2.0 -DryRun
```

只做到"提交并等 CI 绿"就停（不 tag、不发 Release、不推 NuGet）：

```powershell
.\tools\release.ps1 -Version 0.2.0 -SkipPublish
```

注意：脚本不接受脏工作区、不接受的版本号格式、不允许 tag 已存在，
并且要求本地 `main` 与 `origin/main` 同步 —— 这些都在动任何文件之前检查。