# 最近一轮交接：固定开发包目录与历次修复归整

- Owner：Codex-20261018-Package；2026-10-18T01:58:00+08:00开始，02:19:34+08:00完成登记。
- main；起点dc95cae，实现`41da19db4899e4f6959afdf5bd260086484a5e16`。本记录所在提交为报告/任务状态收尾。
- 本轮新增DONE **1项：M5-064**；总表152/176 DONE（M5 52/73），无IN_PROGRESS。M5-H02仍REVIEW，不代签。
- 报告：`Docs/Tasks/Reports/M5-064.md`；修复汇总：`Docs/开发交付与修复汇总.md`。TASKS.md仍为唯一状态来源。

## 本轮实际修改

用户明确要求开发阶段不再制造多版本包。**以后全部只打到`D:\Github-Poj\UE-MMO\Artifacts\Package\Windows`**，同一份覆盖更新。AGENTS.md、执行规则、操作手册和README已同步；Common/Package/TestPackage/TestWeaponGameplay共用固定路径约束，拒绝其他目录。UAT archive根是Artifacts/Package，自动追加Windows；传固定Windows也规范到同一处，不嵌套Windows/Windows。

Run.ps1默认启动固定开发包，-Editor仍打开源码编辑器。包内README.txt为无版本试玩说明，package-info.json记录实际来源提交与exe哈希。历次M5-036、M5-056～063修复、用户失败反馈与当前实现集中在修复汇总；九份历史报告注明旧包退役，原始事实不改写。

10个M5B旧完整包已清理（v3～v12），释放10743914482字节、约10.01GiB，Package下只余Windows。清理前11份包（含旧Windows）的Saved/SaveGames、日志、截图、JSON、说明已逐个SHA校验归档，完整文件清单及exe哈希保存。新目录旧档停在10月6日，已将最后使用包的10月18日Profile_A/B/Index整组迁入；原两套档均归档，迁移后三槽哈希一致。

## 验证与证据

命令使用`rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File`前缀。

| 验证 | 本次结果 |
|---|---|
| Artifacts/Tasks/M5-064/verify-package-paths.ps1 | 16项PowerShell语法、路径正反例、三个入口拒绝版本目录、Run启动路由通过 |
| Scripts/Package.ps1（无目录参数） | BUILD SUCCESSFUL、exit0，固定Windows；package-info.success=true |
| Scripts/TestPackage.ps1 -IncludeWeaponScenario -TaskId M5-064（无目录参数） | 冒烟success/rendering=true；局部4/4、布景1/1、实体2/2、离场1/1、真实菜单到房间八武器8/8 |
| 本次包身份/内容与证据绑定 | 固定exe、非Editor、八武器完整集合、24项原始报告及副本时间/哈希、Binaries/Paks/Data哈希一致 |
| 旧包清理与迁移校验 | cleanup-summary.success=true，10目录移除；save-migration.success=true，3槽迁入后再次核验 |
| 开始/结束CheckTaskBoard | valid=true，无进行中；结构检查不代替玩法检查 |

证据根：`Artifacts/Tasks/M5-064/2026-10-18T01-58-00+0800/`，汇总verification-summary.json success=true；实际八武器`2026-10-18T02-12-38-671+0800-gameplay`。新包启动图、实体飞行与100→90掉血图已查看。此次无C++/资源变化，没有重新声称跑783条全量；其历史结果见M5-063。

只读复核曾指出初版删除门禁只看success；已绑定本次包、报告时效、八武器与内容哈希后再清理，复核无剩余Critical/Important。删除前每个绝对路径都在Package直接子目录内，文件树无链接且未变化；当前Windows不进删除分支。

## 固定交付与接续

试玩`Artifacts/Package/Windows/UEMMO.exe`，或实际exe`Windows/UEMMO/Binaries/Win64/UEMMO.exe`；SHA256 `CC1D14C5E037C6B4C9F16C1483887C7DB1CD4876D9E1A83873EC1006F783BA65`。以后不再发vN目录。源码追溯用Git SHA，原始测试证据按任务时间戳归档。

玩法仍为M5-063可见橙色弹头基线：上方Enter，换枪先I/选行/Equip/Close，向右进房，Q射击、剑X。备弹0且无补给，T拒绝；弹药未持久化，Running禁换装。自动化/截图不代替真实键鼠、声音和手感签收。M5-H02待用户复测，独立可领M5-050；FName大小写摘要差异P2可另卡处理。

原`Scripts/InstallEngineWire.ps1`修改保留未提交，M0-006 PAUSED；M1-H01 BLOCKED，M4 DEFERRED。未改引擎或推送远程。
