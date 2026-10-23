# 迁移到 ylzwd/openaes67 — 操作指南

状态：**本地已全部就绪，只差在新账号登录后执行推送。**

停用原因：原账号 `ylzwd123` 被 GitHub 停用（2026-10 发 v0.1.1 时撞上），远端 403 不可写也不可读。

---

## 已完成（不需要再做任何事）

| 项 | 状态 |
|---|---|
| 合规修复（SPDX 头 /商标声明 / ASIO 许可更新 / 凭据扫描门禁） | ✅ 提交 `58087d5` |
| 仓库内12 处 `ylzwd123` 引用改为 `ylzwd` | ✅ 提交 `d1e2e5c` |
| `git remote` 切到 `https://github.com/ylzwd/openaes67.git` | ✅ 已切换 |
| 失效的 ylzwd123 凭据从keyring 清除 | ✅ 已清除 |
| 完整历史备份 | ✅ `D:\dante项目\_openaes67_backup\openaes67-migration.bundle`（1.2 MB） |
| 源码快照 | ✅ `D:\dante项目\_openaes67_backup\openaes67-source-d1e2e5c.zip`（1.3 MB） |
| 门禁全绿 | ✅ `build.bat` → ALL CLAIMS HOLD + 320 断言 |

当前状态：`main` @ `d1e2e5c`，21 个提交，1 个 tag（`v0.1.0`），工作区干净，
git 对象完好，领先旧 origin 7 个提交。

---

## 你需要做的三步

### 第1 步：在 GitHub 网页建空仓库

打开 https://github.com/new

- Owner 选 **`ylzwd`**
- Repository name 填 **`openaes67`**
- **不要**勾选 Add a README file / .gitignore / License（会导致推送冲突）
- Visibility 选 **Public**（要开源就选这个）
- 点 Create repository

### 第 2 步：本地登录新账号

```powershell
gh auth login -h github.com
```

选 GitHub.com → HTTPS → 用浏览器登录 → 账号是 `ylzwd`。

> 如果 `gh` 浏览器登录不方便，改用 PAT：
> https://github.com/settings/tokens → Generate new token (classic) →
> 勾 `repo` + `write:packages` → 生成后复制，
> 然后 `git remote set-url origin https://<TOKEN>@github.com/ylzwd/openaes67.git`
> （推送完可以把 token 从 remote 里去掉）

### 第 3 步：推送

```powershell
cd D:\dante项目\openaes67
git push -u origin main --follow-tags
```

看到 `main -> main` 和 `v0.1.0` 就成功了。去网页确认仓库内容。

---

## 推送成功后（可选）

**建第一个 Release：**

```powershell
gh release create v0.1.1 --title "v0.1.1" --notes-file notes.md
```

或直接跑项目自带的发版脚本（它幂等可续，CI 绿了才打 tag、自动打包上传）：

```powershell
.\tools\release.ps1 -Version 0.1.1
```

**验证 CI：**

```
gh run list --limit 3
```

CI 有三个 job：build + self-test、policy checks、gui build（best-effort）。

---

## 出问题怎么回滚

完整历史在 bundle 里，任何时候都能恢复：

```powershell
git clone D:\dante项目\_openaes67_backup\openaes67-migration.bundle recovered
```

或者源码快照：

```powershell
Expand-Archive D:\dante项目\_openaes67_backup\openaes67-source-d1e2e5c.zip -DestinationPath recovered-src
```

---

## 保留在报告里的旧账号引用（故意不改）

`docs/COMPLIANCE-REVIEW.md` 里还有两处 `ylzwd123` —— 那是停用事件的调查记录，
改了就失真了。审查报告见 `docs/COMPLIANCE-REVIEW.md`。