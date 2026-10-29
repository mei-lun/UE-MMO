# UE-MMO

UE 5.8.3 单机3D横版学习原型，固定侧面镜头，支持X横向、Y地面纵深移动；使用现成免费或引擎附带素材。

**开发包统一在`D:\Github-Poj\UE-MMO\Artifacts\Package\Windows`。** 后续覆盖同一份，不再创建日期或版本号包。修复追溯使用Git提交和任务报告；完整整理见[开发交付与修复汇总](Docs/开发交付与修复汇总.md)。

当前内容（2026-10-30）：多关卡自由选关（菜单三关：训练场/CombatRoom01/CombatRoom02）；地面载具骑乘链（F8 生成样车，走路带车、Q 车炮、V 下车；armored 变体走表驱动：HP500/防25/速400/挂 12 发线性弹炮塔）；独立主菜单启动图；存档 v3 携带每关通关记录；普通枪、点射和散弹发射独立弹丸实体（橙色可见弹头），参数见`Data/CombatSystem/projectiles.json`。

加内容按[配置作者手册](Docs/Tasks/M5-配置作者手册.md)（武器/子弹/反应/载具/关卡的表驱动步骤都有任务报告背书）。

任务状态只看[TASKS.md](TASKS.md)，最近验证与接续看[HANDOFF.md](HANDOFF.md)。自动验证不等于人工试玩通过；M5-H03（载具试玩）待用户签收，M5-H04 为最终人工关。

## 直接试玩

运行[开发包](Artifacts/Package/Windows/UEMMO.exe)（启动即进主菜单），或在仓库根目录执行：

```powershell
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Run.ps1
```

菜单选一行点 Enter 进关（TrainingArena 训练 / CombatRoom01·02 波次房），方向键左右与纵深移动，C/Space 跳跃，X 普攻，Q 射击（手持武器），I 背包换装，F1 调试面板，F2 训练复位，F8 在身边生成试验载具。

载具骑乘：F8 后走近按 V 上车，方向键走路带车，Q 发射车炮，V 下车。真实键鼠、手感与可听性仍由用户签收（M5-H03/H04）。

## 开发命令

项目`D:\Github-Poj\UE-MMO\UEMMO.uproject`；引擎`D:\Epic\UE_5.8`。Windows原生PowerShell，在仓库根目录执行：

```powershell
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Build.ps1
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1 -Render
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Package.ps1
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestPackage.ps1 -IncludeWeaponScenario
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Run.ps1 -Editor
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckTaskBoard.ps1
```

Package无需传目录，UAT的archive根为Artifacts/Package，自动追加Windows。脚本拒绝其他输出路径；打包后的`Windows/package-info.json`记录提交和exe哈希。验收日志/截图/JSON放`Artifacts/Tasks/<ID>/<timestamp>`，旧包的存档和日志有独立归档，详情见修复汇总。

Run默认启动固定开发包；Run -Editor打开源码编辑器。环境检查、资源准备等命令见[操作手册](Docs/02-准备与环境操作手册.md)。

## 项目文档

- [任务总表](TASKS.md)与[当前交接](HANDOFF.md)：唯一任务状态及接续入口。
- [开发交付与修复汇总](Docs/开发交付与修复汇总.md)：历次武器问题、修复关联、当前结果与未完成项。
- [游戏与技术设计](Docs/01-游戏与技术设计.md)：目标、玩法和接口设计，不把设计当实现。
- [准备与环境操作手册](Docs/02-准备与环境操作手册.md)、[资源登记](Docs/03-资源目录与缺口.md)、[验证记录](Docs/05-验证记录.md)。
- [Agent工作约定](AGENTS.md)、[执行与交接规则](Docs/Tasks/执行与交接规则.md)、[接口约定](Docs/Tasks/接口约定.md)。

角色/动画使用Epic官方模板资产（保留原路径）；音效使用Kenney Impact Sounds（CC0）。装备图标仍有引擎占位，Quaternius候选动作未下载；许可与缺口见资源登记。多人、在线账号与服务端不在当前范围。
