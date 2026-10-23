# OpenAES67 开源合规审查报告

审查日期：2026-10-23
审查对象：`ylzwd123/openaes67` @ `9f269c5`（工作区副本 `D:\dante项目\openaes67`）
审查范围：许可证、第三方知识产权、商标、凭据与隐私、构建产物、GitHub 条款
审查方式：全量代码走查 + `git log --all -p` 历史扫描 + 官方条款比对

---

## 结论摘要

**代码本身的知识产权合规状况优秀，可以开源。但存在 1 个阻塞级外部问题和 3 个需要修的中低风险项。**

| 等级 | 项| 状态 |
|---|---|---|
| 🔴 阻塞 | GitHub 账号当前仍处于**停用**状态，远端 403 | **需立即处理** |
| 🟡 中 | `asio_abi.h` 自行声明 ASIO ABI，性质需重新定性（法规已变） | 建议补说明 |
| 🟡 中 | WDM 驱动测试签名用硬编码密码 `openaes67` | 低风险但应改 |
| 🟡 中 | 全仓**无任何商标声明**，但正文有 281 处 Dante/Audinate 提及 | 建议补Trademark 段 |
| 🟢 低 | `docs/AUDIO-DEVICES.md` 关于 ASIO 许可的表述**已过时**（法规 2025-10 变了） | 文档修正 |
| 🟢 通过 | MIT 许可完整、零厂商二进制、零凭据泄露、零构建产物入库 | 无需处理 |

---

## 一、GitHub 账号停用（🔴 阻塞，最高优先级）

### 实测证据

```
$ git ls-remote --heads origin
remote: Your account is suspended. Please visit https://support.github.com for more information.
fatal: unable to access 'https://github.com/ylzwd123/openaes67.git/': 403
```

**这不是历史记录，是当前状态。** `docs/FINDINGS.md:8828` 记录了停用经过（发v0.1.1 时撞上），此后未恢复。

### 由此产生的实际问题

| 问题 | 现状 |
|---|---|
| 远端不可写| 任何提交、tag、Release、NuGet 推送全部失败 |
| 仓库实际可见性未知 | 被停用的仓库通常会转为私有；**若仓库已转私有而你以为是公开，开源就无从谈起** |
| v0.1.1 未发布 | 代码自称 `0.1.1`，公开可见的最新 Release 是 `0.1.0` |
| CI 无法运行 | 恢复后需补跑 `tools/release.ps1 -Version 0.1.1`（脚本设计成可续跑，无需重做） |

### 建议动作

1. **先确认仓库可见性**：登录后看 `Settings → General → Danger Zone` 中Visibility 是Public 还是 Private。
2. **向 GitHub 申诉**：`https://support.github.com/contact`，附上项目说明（纯 MIT、零厂商代码、无自动化行为）。
3. 若短期内无法解封，**先把仓库迁移/镜像到新账号**，保住开源成果。git 历史干净，无凭据，迁移零成本：
   ```bash
   # 在新账号下建空仓库后
   git remote set-url origin https://github.com/<新账号>/openaes67.git
   git push -u origin main --follow-tags
   ```
4. 恢复后先跑一次 `.\tools\scan-secrets.ps1` 再发布（脚本存在，且当前实测通过）。

---

## 二、ASIO ABI 声明的合规定性（🟡 中）——法规已变，文档未跟上

这是本次审查**最需要更新认知**的一点。

### 事实变化

| 时间 | ASIO 许可状态 |
|---|---|
| ~2005 起 | Steinberg **专有**许可，明确禁止再分发 SDK |
| **2025-10-15** | Steinberg 宣布 ASIO **双许可**：GPLv3 **或** 继续走原专有许可 |
| 现在（2026-10） | GPLv3 变体正式提供，SDK 可免费下载 |

官方原文（steinberg.net/developers/asiosdk-open）：
> "Steinberg ASIO technology is available in open source form under a GNU General Public License, version GPLv3."
>
> "**Can I switch to GPLv3 license?** Yes, **if your product is released under GPLv3 license terms** (including source code disclosure)."

### 本项目的做法

`asio_driver/asio_abi.h` 自行重写了一遍 ASIO 接口声明，理由写在文件头：
> "An ASIO driver is, however, just a COM in-process server whose object implements one abstract class (IASIO) and whose **vtable layout is a published ABI contract**: hosts call through it by offset, not by header."

### 评估：做法本身站得住，但有一个必须澄清的点

**站得住的部分：**

1. **仓库不含任何 Steinberg 源文件。** 全历史扫描确认 `third_party/` 下只有 `README.md`，从未提交过 `iasiodrv.h` / `asio.h` / `asiosys.h`。
2. **接口声明 ≠ 实现代码。** `asio_abi.h` 声明的是 ABI 契约（类型、结构体布局、方法顺序），不含任何 Steinberg 的实现逻辑。这是互操作性所必需的，与描述一个网络协议结构体同理。
3. **官方路径优先。** `asio_abi.h` 开头就是 `#if defined(OPENAES67_USE_OFFICIAL_ASIO_SDK)` → 走官方头文件；注释明确写"if they ever disagree, the official one wins"。这是业界通行的双轨做法。
4. **COM GUID 是自造的。** `kClassId = {0x7c1f2e64, ...}` 是本项目自己的 CLSID，**不是** Steinberg 的 `IID_IASIO`。没有冒用官方标识符。

**需要澄清的点：**

旧专有许可的2.3 条款曾明确写"**This includes re-working this specification**"。本文件虽然只字未抄官方头文件，但**属于对接口规范的重述**，在旧法规语境下存在被质疑的空间。

GPLv3 变体把这个顾虑基本消解了——但**有一个前提必须点明**：

> **如果使用官方 ASIO SDK 的 GPLv3 版本，整个项目必须改为 GPLv3**（copyleft 传染）。

本项目是 **MIT**，因此**没有使用 GPLv3 SDK**，走的是「自写 ABI 声明」这条路。这在逻辑上是自洽的（不引入 GPLv3 代码，所以 MIT 成立）。

### 建议动作

1. **修正 `docs/AUDIO-DEVICES.md:78` 的过时表述**。当前写"ASIO SDK 不可再分发"已不准确，应补充：ASIO 现有 GPLv3 变体，但 GPLv3 的 copyleft 会传染到整个项目，故本项目（MIT）仍选择不引入官方 SDK，改用自声明 ABI。
2. **在 `asio_abi.h` 增加一句法规出处**，写明"ASIO 自 2025-10 起提供 GPLv3 变体；本项目为 MIT，未采用，故不引入任何官方 SDK 文件"。这让审查者一眼看到是主动决策而非疏漏。
3. **补充商标说明**：ASIO 名字与 logo 是 Steinberg 注册商标（官方明确"GPL 不覆盖商标"）。本项目在 README/代码中大量使用 "ASIO" 一词作为技术描述（ nominative use，描述兼容性），这属于允许的用法。但应加一句明示不含商标许可。

---

## 三、商标使用（🟡 中，建议补强）

### 现状

- 全仓 **281 处** 提及 Dante / Audinate
- README 有 "the project is not affiliated with any vendor"、"it is not a Dante device"（**表述正确**）
- 有Dante 官方文档的引用链接（`docs/DANTE-INTEROP.md`，属合理引用）
- **但全仓找不到任何一处 `trademark` / `商标` 声明**

### 风险评估：低，但存在

| 商标 | 使用方式 | 判定 |
|---|---|---|
| **ASIO** | 描述性用法："ASIO 64×64 声卡"、"this build has no ASIO backend" | ✅ nominative use，安全。但官方商标政策未明示声明 |
| **Dante** | 描述性用法：讲如何与 Dante 设备互操作 | ✅ nominative use，安全 |
| **Audinate** | 仅在引用官方文档与说明非关联时出现 | ✅ 安全 |
| ASIO **logo** | **未使用**（全部图标由 `logo.png` 自生成，`assets/README.md` 可证） | ✅ 无风险 |
| Dante **logo** | 未使用 | ✅ 无风险 |

关键点：**项目没有使用任何厂商的 logo 或图形标识**，全部图标自绘自生成。这消除了商标侵权的主要风险源。

### 建议动作

在 README.md / README.zh-CN.md 加一个简短的商标声明段（不需要律师，10行以内即可）：

```markdown
## Trademarks

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
Dante and Audinate are trademarks of Audinate, Ltd.
This project is not affiliated with, endorsed by, or sponsored by any of them;
the names are used only to describe interoperability.
OpenAES67 is not a Dante device.
```

---

## 四、凭据与隐私（🟢 通过）

### 全历史扫描结果：干净

| 检查项 | 结果 |
|---|---|
| GitHub token（`ghp_` / `github_pat_`） | ✅ 全历史 0 命中 |
| AWS key（`AKIA...`） | ✅ 0 命中 |
| 私钥块（各类 PEM 私钥头） | ✅ 0 命中 |
| `.pfx` / `.cer` / `.pvk` / `.key` / `.pem` / `.snk` | ✅ **历史上从未提交过任何一个** |
| `.exe` / `.dll` / `.sys` / `.cat` | ✅ 历史上从未提交过 |
| 真实内网 IP | ✅ 仅 `10.1.2.3`、`10.99.99.99`，均为 RFC 1918 段构造的测试值 |

`.gitignore` 第 32-39 行专门写了注释说明私钥排除规则，且覆盖 `.pfx/.cer/.pvk/.snk/.key/.pem` 六类 —— 说明作者是被坑过或提前防住了。

仓库另有两个自动化防线：`tools/scan-secrets.ps1`（凭据与本机路径扫描）和 `tools/verify-perf.ps1`（性能门禁）。

### 唯一小瑕疵：驱动测试签名用硬编码密码

`driver/build-driver.bat:97`：
```powershell
Export-PfxCertificate -Cert $c -FilePath '%PFX%' `
  -Password (ConvertTo-SecureString -String 'openaes67' -Force -AsPlainText)
```

**判定：低风险，不构成违规。** 理由：
1. 这是**临时自签测试证书**（`New-SelfSignedCertificate`，2 年有效期），仅用于本机测试签名
2. PFX 文件本身已被 `.gitignore` 排除，永不入库
3. 密码是公开字符串 `openaes67`，不保护任何有价值的东西
4. 真实签名证书（`.pfx`）走的是另一条路径，需要开发者自己的证书

但硬编码密码会触发静态扫描工具的告警，也可能被误认为泄露。**建议**改为从环境变量读取，或直接用 `-ProtectTo 'CN=...'` 让 PowerShell 走DPAPI：
```powershell
-Password (ConvertTo-SecureString -String $env:OPENAES67_PFX_PASS -Force -AsPlainText)
```

---

## 五、构建产物与仓库卫生（🟢 通过）

| 检查项 | 结果 |
|---|---|
| 被追踪文件总数 | 147 |
| `.exe` / `.dll` / `.sys` / `.obj` / `.nupkg` 入库 | ✅ **0 个** |
| `build/`、`build-qt/`、`driver/out/` | ✅ 已被 `.gitignore` 排除（本地存在但未入库，正确） |
| 第三方代码 | ✅ `third_party/` 仅一个说明 README，无任何二进制或源码 |
| 图标/Logo 来源 | ✅ 全部由 `logo.png` 经 `tools/make-icon.ps1` 自生成，`check-icons.ps1` 独立校验 |
| GitHub Actions 产物 | ✅ 走 artifact 上传，不入 git |

`.gitignore` 写得很有质量：注释解释了**为什么**（"ASIO SDK is not redistributable; only the placeholder README is tracked"），而不是只列规则。

---

## 六、MIT 许可与代码来源（🟢 通过）

- `LICENSE` 是**完整未删减的 MIT 全文**（21 行），非"MIT License"标题占位式
- 版权行 `Copyright (c) 2026 OpenAES67 contributors` —— 集体署名，无冒用他人身份
- CI 有 policy job 强制扫描厂商标识符与许可/激活逻辑，两条都是**真门禁**（exit 1）
- 全仓 `src/` + `include/` **零 TODO/FIXME/HACK**
- 未发现任何来源不明的粘贴代码片段

一个可以考虑的小改进：MIT 许可严格要求"The above copyright notice ... shall be included in all copies"。当前源文件里**没有 SPDX 头**。虽然有顶层 LICENSE 通常足够，但加 SPDX 头能让文件在脱离仓库后仍自证许可，是社区惯例。

---

## 七、优先级行动清单

| # | 动作 | 优先级 | 说明 |
|---|---|---|---|
| 1 | 确认仓库可见性并申诉 / 迁移账号 | 🔴 **立即** | 账号仍 403，开源成果当前实际不可访问 |
| 2 | 修正 `docs/AUDIO-DEVICES.md` 中 ASIO 许可表述 | 🟡 本周 | 补 2025-10 GPLv3 变更 + 为何本项目仍不引入 |
| 3 | 在 `asio_abi.h` 补法规出处说明 | 🟡 本周 | 一句话，让审查者看到是主动决策 |
| 4 | README 加商标声明段（中英双语） | 🟡 本周 | 10 行内，ASIO/Dante/Audinate 均为描述性使用 |
| 5 | 驱动测试签名密码改环境变量 | 🟢 有空再做 | 避免静态扫描误报 |
| 6 | （可选）源文件加 SPDX 头 | 🟢 有空再做 | MIT 合规最佳实践 |
| 7 | 恢复后补跑 `release.ps1 -Version 0.1.1` | ⏸ 待解封 | 脚本已设计为可续跑 |

---

## 附：一句话总结

**代码干净、许可干净、历史干净 —— 可以放心开源。** 唯一挡在"公开可访问"前面的不是代码问题，是 GitHub 账号仍被停用。代码侧唯一值得补的是文档：ASIO 许可规则在 2025 年 10 月变了，`docs/` 里的表述需要跟上，另外补一段商标声明就能让合规面完整闭合。

---

## 附二：已执行的修复（2026-10-23）

代码侧全部修完并验证。**唯一未解决项是 GitHub 账号停用，那不是代码问题。**

| # | 修复 | 文件 | 验证 |
|---|---|---|---|
| 1 | 修正过时的 ASIO 许可表述，补 2025-10 GPLv3 变更与「MIT 项目为何仍不引入官方 SDK」的决策理由 | `docs/AUDIO-DEVICES.md` | 人工复核 |
| 2 | 补法规出处：GPLv3copyleft 会传染整个项目，故自声明 ABI | `asio_driver/asio_abi.h` | ASIO 构建 + host test ALL PASS |
| 3 | 新增 Trademarks 声明（ASIO / Dante / Audinate 均为描述性使用，无厂商 logo） | `README.md`、`README.zh-CN.md` | 人工复核 |
| 4 | 驱动测试签名硬编码密码改为读环境变量 `OPENAES67_TEST_PFX_PASS`（3处） | `driver/build-driver.bat` | 门禁 no secrets PASS |
| 5 | **59 个源文件补 SPDX 许可头**（MIT 要求版权声明随代码分发） | `src/`、`include/`、`tests/`、`asio_driver/`、`driver/` | `build.bat` → ALL CLAIMS HOLD，自测 320 断言通过 |
| 6 | 新增幂等的 SPDX 头生成脚本，供后续新增文件使用 | `tools/add-spdx-headers.ps1` | 重跑输出 `0 added; 59 already had one` |
| 7 | **`.workbuddy/` 加入 `.gitignore`** —— 修复一个真实泄露风险：agent 的会话笔记会被提交进开源仓库 | `.gitignore` | `git status` 已不再显示 |

### 修复过程中被项目自己的门禁抓到一次

第5 项完成后跑 `build.bat`，`no secrets or local paths in the tree` 报FAIL：

```
[FAIL] no secrets or local paths in the tree
  docs\COMPLIANCE-REVIEW.md:154  <PEM 私钥头字面量>
```

原因是我在本报告里**逐字写出了私钥特征串**用于说明「已扫描过」——扫描器按字面匹配，命中了。

这正是 `CONTRIBUTING.md` 里那条教训的翻版：**「文档里不要复述敏感值」**。机器能查的交给 `scan-secrets.ps1`，人工只写结论。已改为「各类PEM 私钥头」，重跑 `build.bat` → `ALL CLAIMS HOLD`。

顺带说明：`build.bat` 的门禁设计在这里救了场——它不仅拦厂商标识符，也拦自己人写的文档。

### 最终验证记录

```
$ ./build.bat                          → ALL CLAIMS HOLD / build OK
  ├─ 17 项claim 门禁                全部 PASS
  └─ self-test                    320 assertions, ALL PASS

$ ./asio_driver/build-asio-driver.bat   → BUILD OK
$ ./asio_driver/out/asio_host_test.exe  → ALL PASS
$ tools/scan-secrets.ps1                → no credentials, no local paths
```

**代码侧已无待修项。** 解封账号或迁移到新账号后即可直接发布。