# 当前源码重新构建与试玩包交接（2026-11-04）

- Owner：Codex-20261104-PlaytestBuild；执行时间：2026-11-04T09:46:48+08:00～2026-11-04T09:55:37+08:00（验证摘要生成时间）。
- 用户要求：“先帮我重新构建，我体验看下”。本轮新增 DONE **0 项**，没有接管/完成 M6-013 或 M6-015，也没有代签人工验收。
- 分支/起止 HEAD：main / `26a625317bd44ab00caf03a2140f40931350350d`；**构建包含该提交之外既有未提交工作区改动**。没有新功能提交、推送或美术修复。
- 开发包唯一目录：`D:\Github-Poj\UE-MMO\Artifacts\Package\Windows`；生成时间 `2026-11-04T09:52:20.6105694+08:00`。
- exe SHA256：`FEDB047100237C9DE35C6B69C9E4757B0751EDDDCF5E2E9C44F9B01136FBB42B`，本轮重算与 package-info.json 一致。
- 原始证据：`Artifacts/Tasks/M6-015/2026-11-04T09-46-48+0800-playtest-build/`。

## 实际操作与结果

所有命令在仓库根目录通过 `rtk proxy` 执行。

| 验证/操作 | 结果 |
|---|---|
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Build.ps1` | exit=0；UBT `Result: Succeeded`，保留 build-Editor.log/json |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Package.ps1` | exit=0；`BUILD SUCCESSFUL`、UAT ExitCode=0；新 package-info.json success=true |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestPackage.ps1 -TaskId M6-015` | exit=0；新 package-smoke.json success=true、rendering=true，X=421.026cm、Y=-463.871cm、跳高=130.746cm；菜单截图已查看，包含 GuildagitScene |
| 包内直接加载 `/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit`，加 `-UEMMOSmoke -unattended -RenderOffscreen -nosound -ResX=1280 -ResY=720` | **FAIL**；地图完成加载且渲染成功，但 guildagit-smoke.json success=false，X=-1.073cm、Y=0cm、跳高=130.748cm。截图已查看，菜单仍覆盖在场景上，灰色场景与 Quinn 敌人可见；不能把这次直接加载当作菜单进关、正常走位或玩家视觉验收通过 |
| 存档保护 | 重建前备份包内 Saved/Artifacts、旧 package-info，以及存在的 LocalAppData 存档；测试后按原存档字节核验/必要时恢复，7 个既有存档均保留。具体逐文件结果见 save-preservation.json |
| 启动用户试玩 | 执行 `Scripts/Run.ps1`，启动固定目录的新包 |
| 任务板检查 | 开始仍 exit=1：M6-013/M6-015 同目录双 IN_PROGRESS；结束同样检查，原任务状态与 Owner 未由本轮改写 |

`playtest-build-summary.json` 明确区分 build_success/package_success/startup_smoke_success=true 与 guildagit_smoke_success=false、all_checked_scenarios_passed=false。本次交付为用户查看当前效果的试玩包，不能称为全部验收通过。

## 工作区追溯与限制

证据目录的 source-working-tree.patch、git-status-before.txt、build-workspace.json 记录本次使用的未提交差异及关键代码/地图/格斗家资产哈希。固定包根也放了 build-workspace.json，避免将 package-info 中 HEAD 误当成完整源码状态。

旧审计关于“固定包仍为 11 月 3 日”的描述已被本轮新构建取代；其余缺图、占位地形、只有待机、敌人和图标未全部替换、双房地城生产接线未完成等结论仍然有效。

下一步由用户体验当前包；原负责人后续收尾 M6-013/M6-015，特别是菜单进出公会图、地图移动失败原因、2D 角色实际画面与动作限制。未经验证不得标 DONE。用户存档没有通过清除来制造首次运行成功。
