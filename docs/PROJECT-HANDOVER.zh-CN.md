# OpenAES67 项目接管记录

检查日期：2026-10-25（北京时间）。分析起点：`main` 的 `462a47f`。

## 项目现状

这是一个 Windows C++17 网络音频项目，包含命令行工具、Qt 6 界面和用户态 64 进 64 出 ASIO 虚拟声卡。主要功能已有实现，也有编译、自检、性能测量及发布工具，可以在现有代码上继续维护。

本次开始时工作区干净，本地 `main` 与 GitHub `origin/main` 相同。仓库为公开仓库，当前登录账号具备管理权限；已有 `v0.1.1` 发布版。分析起点比 `v0.1.1` 标签多 8 个提交，因此当前源码与已发布二进制应分别核对。

工作区分为两个目录：

| 目录 | 用途 |
|---|---|
| `openaes67/` | 当前 Git 仓库，后续开发和上传以此为准 |
| `_openaes67_backup/`（仓库旁） | 旧 Git bundle、源码快照和发布压缩包；其中 bundle 的 `main` 停在 `d1e2e5c`，不能作为最新源码备份 |

## 主要结构

| 入口 | 职责 |
|---|---|
| `src/main.cpp` | CLI 参数、发送、接收、诊断与工具命令 |
| `src/gui/main_gui.cpp` | Qt 界面、工作线程、托盘与设备操作入口 |
| `src/receive_loop.cpp` | CLI、GUI、bridge 共用的接收、缓冲和播放流程 |
| `src/bridge.cpp` | 网络音频与 ASIO 共享环的双向桥接 |
| `src/engine.cpp`、`src/net.cpp` | RTP 收发、UDP 套接字及网络配置 |
| `src/rtp.cpp`、`src/sdp.cpp`、`src/sap.cpp`、`src/rtcp.cpp` | 媒体封装、流描述、发现和报告 |
| `src/ptp.cpp`、`src/resampler.cpp`、`src/jitter.cpp` | 时钟跟踪、速率控制及抖动缓冲 |
| `src/audio_io_*.cpp` | WASAPI、独占播放、WAV 及可选 ASIO 宿主后端 |
| `asio_driver/` | 用户态 ASIO COM DLL 和独立宿主测试程序 |
| `driver/` | 实验性 WDM/PortCls 内核驱动及安装工具 |
| `tests/selftest.cpp` | 协议、缓冲、配置等自检 |
| `tools/`、`.github/workflows/ci.yml` | 本地校验、性能与稳定性测量、打包发布、云端构建 |

核心声卡数据路径：

```text
网络 RTP → 接收引擎/缓冲/速率控制 → 共享环 render → ASIO 输入 → DAW
网络 RTP ← 发送引擎              ← 共享环 capture ← ASIO 输出 ← DAW
```

`asio_driver/` 的虚拟声卡与 `src/audio_io_asio.cpp` 的宿主后端是两个模块。前者默认使用仓库内的 ABI 声明编译；后者是需要单独配置 SDK 的可选构建路径。本次没有验证可选 SDK 构建。

## 本次验证

| 项目 | 结果与范围 |
|---|---|
| 直接 MSVC 构建 | CLI、自检程序重新编译完成；声明校验串行复查通过 |
| 核心自检 | 321 项通过，`ALL PASS (0 failures)` |
| 声明一致性校验 | `tools/verify-claims.ps1` 的 31 项检查通过 |
| 敏感信息扫描 | `tools/scan-secrets.ps1` 在其现有规则覆盖范围内无发现 |
| CMake / Qt 构建 | 现有 `build-qt` 配置成功生成 CLI、核心库、GUI、自检程序；其自检 321 项通过 |
| ASIO 声卡 | DLL 与宿主测试程序重新编译成功，2 秒宿主测试 `ALL PASS` |
| 起点提交的 GitHub Actions | [运行 40084366144](https://github.com/ylzwd/openaes67/actions/runs/40084366144) 的 Windows 构建、策略、GUI、WDK 四个任务均成功 |
| GitHub 问题与 PR | 检查时没有打开的 issue 或 PR |

首次并发运行两套自检时出现单项失败，随后两套自检分别串行运行均通过。测试使用同一个临时目录和固定文件名，存在并发争用风险；后续应串行执行，若要并发执行则先隔离测试数据。

ASIO 宿主测试证明 DLL、接口、回调和共享环可工作；本次没有连接真实 Dante 设备或 DAW 做端到端音频测试，没有复测 README 中的历史长跑数据，也没有安装或加载内核驱动。

## GitHub 连接与日常操作

- 仓库：[ylzwd/openaes67](https://github.com/ylzwd/openaes67)。
- 分支：`main`，跟踪 `origin/main`。
- 认证：现有 GitHub CLI / 系统凭据管理器。
- 本机 Git 直连曾超时，已验证现有本机代理可访问远端，并在本仓库的 `.git/config` 中设置 GitHub 代理；代理服务需要保持可用，配置不会随源码上传。
- 若换到可直连的网络，可执行 `git config --local --unset http.https://github.com/.proxy` 清除这项本机配置。

常用命令均在仓库根目录执行：

```powershell
git status --short --branch
git fetch origin
git log --oneline origin/main..HEAD
gh run list --repo ylzwd/openaes67 --limit 5
```

修改后按范围运行构建、自检和扫描，明确暂存文件，再提交并推送。推送后核对远端提交 SHA 及对应 Actions 任务；单看工作流总状态不足以判断允许失败的 GUI 和 WDK 任务。

发布二进制使用既有 `tools/release.ps1` 流程。普通源码上传不修改版本标签，也不替换现有 Release 附件。

## 待处理问题与优先级

以下为代码检查发现的维护事项，本次接管未修改音频实现或日报脚本：

| 优先级 | 事项 | 依据与建议 |
|---|---|---|
| 高 | 日报可能误报设备诊断成功 | `tools/daily-check.ps1` 使用 `-match 'READY'`，也会匹配 `NOT READY`。应同时校验进程退出码和明确的成功行，并用失败输出验证 |
| 高 | 日报提交与公开数据边界 | 同一脚本把主机名和真实网卡地址写进报告，暂存整个 `docs/daily`，且未检查 Git 操作退出码。应脱敏、只暂存目标报告，并传播提交/推送失败 |
| 中 | 日报断言数量过时 | 脚本固定写 320 项，当前自检为 321 项；应从输出统计，历史日报不作为当前验证结果 |
| 中 | 高通道数的网络互通限制 | `StreamConfig::payloadBytes()` 下，48 kHz、64 通道、1 ms 的 L16/L24 载荷分别为 6144/9216 字节。应结合网络 MTU 与目标设备做实际测试；64 路声卡能力不能直接等同于单条网络流的互通能力 |
| 中 | 内部音频精度 | `include/openaes67/config.h` 和共享环使用 int16；L24 网络封装并不代表内部保留完整 24 位精度 |
| 中 | PTP 与驱动交付限制 | 现有文档记录软件时间戳和 Secure Boot 限制，后续交付以 ASIO 路径为主；详见 `AES67-CONFORMANCE.md`、`M6-FEASIBILITY.md` |
| 中 | 发布版与主分支差异 | 下次发布前核对版本资源、签名、许可证附件和源码标签，不以本机生成文件替代已验证的 CI 产物 |

本次已修正 ASIO 子目录中“驱动未实现、必须先有 SDK”的过时说明，将迁移指南更新为已完成状态，并修正两个 README 的 GUI 构建示例。维护约定保存在仓库根目录的 `AGENTS.md`，便于后续继续处理这些事项。
