# UE-MMO

UE 5.8.3 单机横版动作学习原型，支持 X 横向、Y 地面纵深移动和跳跃。

**当前交付：最新替换资源体验包（M6-026）。** 双击 [UEMMO.exe](Artifacts/Package/Windows/UEMMO.exe) 即自动进入公会场景，使用最新 DNFM 格斗家动作，六技能已准备好。正常菜单只保留同一体验入口。

开发包固定在 D:\Github-Poj\UE-MMO\Artifacts\Package\Windows，后续更新覆盖同一份。也可运行 Scripts/Run.ps1；-Menu 打开单入口菜单，-Editor 打开工程。

## 操作

- 方向键：左右及前后纵深移动；C / 空格：跳跃。
- X：普攻；Z：上挑；W：单踢；E：下段踢；R：抛沙；A：地裂；S：踏地。
- F2：复位、回复生命和 MP、清除技能冷却；I：背包；Alt+F4：退出。
- 清敌后领取奖励并返回菜单，可再次进入体验。

本体验使用独立 ResourceExperience_ 存档槽，领取奖励时保存；未领取即退出不保存本轮进度。原 Profile_/Roster_/Char_ 存档保留。

## 当前边界

新格斗家衣装与跑跳、攻击、受击、倒地、起身、死亡动作已接入。公会地图的原色贴图和地形仍未补齐，场景还有灰色占位；普通敌人和部分图标也仍有占位。当前不代表完整美术交付、多职业玩家流程或连续地城已完成。

单机战斗、波次、成长、背包、装备、存档，以及武器和地面载具底层已有实现。历史开发测试地图保留供自动化验证，不出现在正常体验菜单。多人、在线账号和服务端未实现。

任务状态只看 [TASKS.md](TASKS.md)，接续见 [HANDOFF.md](HANDOFF.md)。自动化不能代替用户试玩签收。

## 开发验证

仓库根目录使用 Windows PowerShell，所有命令加 rtk proxy 前缀：

- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Build.ps1
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1 -Render
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestAutomation.ps1 -Filter UEMMO.Tasks -TaskId YOUR_TASK_ID
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Package.ps1
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestPackage.ps1
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestResourceExperience.ps1 -Package
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckTaskBoard.ps1

Package 的 UAT archive 根是 Artifacts/Package，由 UAT 追加 Windows；不能再建日期、vN 或 latest 包目录。package-info.json 记录源码提交与 exe 哈希；日志、截图和 JSON 留在 Artifacts/Tasks/<ID>。

## 文档

- [DNFM资源替换与最小复刻计划](Docs/DNF-Research/07-DNFM资源替换与最小复刻计划.md)：三角色、两地图、小怪、Boss及完整战斗的实施范围、源资源核查与验收标准；计划不等于已实现。
- [本轮体验包报告](Docs/Tasks/Reports/M6-026.md)与[试玩说明](Docs/DevelopmentPlaytest.txt)。
- [项目审计](Docs/Audit-2026-11-06.md)：本次入口收敛前的进度快照。
- [开发交付与修复汇总](Docs/开发交付与修复汇总.md)。
- [游戏与技术设计](Docs/01-游戏与技术设计.md)、[操作手册](Docs/02-准备与环境操作手册.md)、[资源登记](Docs/03-资源目录与缺口.md)、[验证记录](Docs/05-验证记录.md)。
- [Agent 约定](AGENTS.md)、[执行与交接规则](Docs/Tasks/执行与交接规则.md)、[接口约定](Docs/Tasks/接口约定.md)、[配置作者手册](Docs/Tasks/M5-配置作者手册.md)。
- [DNFM 资源接手入口](Docs/DNF-Research/06-Agent资源接手入口.md)：279 个静态模型、137 个来源材质和39 张纹理的本地接手信息与缺失项。
