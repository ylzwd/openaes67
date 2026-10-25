# OpenAES67 维护入口

## 项目与语言

- 默认使用简体中文与用户交流；新文档默认简体中文，已有英文文档按其原语言维护。
- 先阅读 `README.zh-CN.md`、`CONTRIBUTING.md` 和 `docs/PROJECT-HANDOVER.zh-CN.md`。
- 本仓库是 Windows C++17 网络音频项目，主要交付 CLI、Qt 6 GUI 和用户态 64×64 ASIO 声卡。
- 遵循已有协议实现、MIT 许可和可复现测量要求，不把文档中的历史测量当成本次实测。

## 代码入口

- `src/main.cpp`：CLI；`src/gui/main_gui.cpp`：Qt 界面。
- `src/receive_loop.cpp`：CLI 与 GUI 共用的接收流程，保持共用实现。
- `src/bridge.cpp`：双向网络与共享内存桥接。
- `include/openaes67/`：公共接口；`src/`：协议、缓冲、时钟及音频后端。
- `asio_driver/`：用户态 ASIO 声卡；`driver/`：实验性 WDM/PortCls 内核驱动，两者不要混淆。
- `asio_driver/asio_abi.h` 支持默认声卡构建；可选的宿主 ASIO 后端仍由 `OPENAES67_WITH_ASIO` 和 SDK 配置控制。

## 构建与验证

在仓库根目录使用 PowerShell：

```powershell
.\build.bat
.\asio_driver\build-asio-driver.bat
.\asio_driver\out\asio_host_test.exe 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools\scan-secrets.ps1
```

- `build.bat` 编译 CLI、自检程序，并执行 `tools/verify-claims.ps1`。
- CMake 构建使用 Visual Studio 2022 x64；Qt 6 通过 `CMAKE_PREFIX_PATH` 查找，找到后才生成 GUI 目标。
- CMake 自检程序为 `<构建目录>/Release/openaes67_selftest.exe`；直接构建的自检程序为 `build/selftest.exe`。
- 自检使用固定临时文件名，串行运行各构建路径的自检和声明校验，避免并发争用。
- 改动收包循环、共享环、ASIO 泵等热路径时，执行 `tools/verify-perf.ps1`；性能结论必须附测量条件。
- 按变更范围执行验证；文档改动检查链接、命令、声明及敏感信息，音频改动补充相应的实际链路测量。
- `tools/daily-check.ps1` 的已知问题记录在接管文档中；不要仅凭其摘要认定检查通过。

## GitHub 与发布

- 远端为 `https://github.com/ylzwd/openaes67.git`，默认分支为 `main`。
- 操作前检查工作区、远端差异及用户授权范围；只暂存当前任务涉及的文件。
- 推送后核对远端提交，并查看该提交的 GitHub Actions 结果；GUI、WDK 任务允许失败，需单独查看任务结果。
- 项目凭据使用 GitHub CLI 或系统凭据管理器；不把令牌写进远端 URL、源码、日志或文档。
- 本机网络配置保存在 `.git/config`，不要把代理地址、真实网卡地址、主机名或个人目录加入公开文档。
- 不上传构建输出、签名材料、外部 ASIO SDK 或工作区旁的备份目录。
- 普通源码上传不等于发版；用户要求发版时使用 `tools/release.ps1`，按 `version.h` 和已有版本一致性校验维护版本。
