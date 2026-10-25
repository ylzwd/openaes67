# GitHub 迁移状态：已完成

更新日期：2026-10-25（北京时间）。

当前仓库为 [ylzwd/openaes67](https://github.com/ylzwd/openaes67)，本地 `main` 已跟踪 `origin/main`。已验证登录账号具备仓库管理权限，远端可读取，已有 `v0.1.0`、`v0.1.1` 发布记录。无需再次创建仓库、重做首次推送或重建已有标签。

项目结构、验证结果、本机网络配置说明和后续维护事项见
[`PROJECT-HANDOVER.zh-CN.md`](PROJECT-HANDOVER.zh-CN.md)。

## 日常同步

在仓库根目录执行：

```powershell
gh auth status
git status --short --branch
git remote -v
git fetch origin
git rev-list --left-right --count HEAD...origin/main
```

最后一条命令的两个数字分别为本地领先和落后的提交数。均为 0 表示已同步；若存在远端新增提交，先检查差异，再决定如何整合。

提交当前任务的变更并通过项目校验后，使用：

```powershell
git push origin main
gh run list --repo ylzwd/openaes67 --limit 5
```

凭据通过 GitHub CLI 或系统凭据管理器保存，不把令牌放进远端 URL。需要重新登录时使用 `gh auth login -h github.com`，并保留不含凭据的远端地址 `https://github.com/ylzwd/openaes67.git`。

## 历史备份

仓库旁的 `_openaes67_backup/` 保留迁移时的 Git bundle、源码快照和旧发布附件。bundle 的 `main` 指向 `d1e2e5c`，只是迁移时的恢复点，不包含后续所有维护提交。

如需检查或恢复，选择一个新的空目录，在当前仓库根目录执行：

```powershell
git bundle verify ..\_openaes67_backup\openaes67-migration.bundle
git clone ..\_openaes67_backup\openaes67-migration.bundle recovered
```

恢复出的目录用于核对旧版本，不覆盖当前仓库。备份文件和本机构建结果不纳入源码推送。

## 版本发布

源码同步与二进制发布分别处理。需要发布新版本时，按 `CONTRIBUTING.md` 使用 `tools/release.ps1`，让标签对应通过 CI 的提交。现有版本和附件不因重新接管而改写。
