# UE-MMO

UE 5.8.3 单机 3D 横版研究工程：固定侧面镜头，支持 X/Y 地面纵深移动，使用现成免费或引擎附带素材，由编程 Agent 完成客户端实现。

**当前版本：M2 技术完成 / 待用户验收（M2-H01）。** M0 房间、Manny 玩家、左右/纵深移动、跳跃、复位，M1 客户端战斗技术栈（X 普攻二段、Z 上挑、跳取消后空中追击、伤害/浮空/倒地恢复、训练假人与会话复位、调试面板 F1）之外，M2 已实现近战敌人（追击与纵深对齐、前摇攻击、受击死亡）、刷怪房间会话（激活区进房、两波刷怪推进、玩家死亡失败重试、中途退出清理）、胜利/失败结果界面（Retry/Return 按钮）与出口红/绿解锁。M2 全部 15 项技术任务已通过自动验证（329 条自动化测试全绿、两波刷怪完整场景回归、Development 独立包内验证：两波清关/死亡重试/中途退出场景在打包 exe 内跑通并留事件 JSON）。按项目规则，M2 阶段验收（M2-H01）必须由用户实际试玩后确认，任何 AI 不得代签。注意：游戏内自动开波属于后续接线任务——当前试玩包内两波刷怪以包内自动化取证，结果界面/重试/退出在游戏内用调试命令驱动（见下面试玩步骤）；装备成长仍未开始，联网仍是后续设计目标。

## 直接打开

- 工程：`D:\Github-Poj\UE-MMO\UEMMO.uproject`。
- UE：`D:\Epic\UE_5.8`。
- ZCode：`D:\ZCode\ZCode.exe`（已安装，用户自行登录后打开此目录）。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Run.ps1 -Editor
```

直接试玩：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Run.ps1
```

按键：←/→ 方向键左右移动，↑/↓ 方向键纵深移动，X 普攻，Z 上挑，C 跳跃（Space 保留为跳跃别名），F2 回到出生点（训练房会话复位），Q W E R A S D F 为技能槽 1-8（预留：当前按下仅产生技能槽意图，无技能效果），F1 开关战斗调试面板。完整连招（先按 F1 打开调试面板观察）：走近假人 → X（light_01）→ 约两次连续 X 的节奏内再按 X 链 light_02 → 按 Z 出上挑 → 上挑命中后按 C 跳取消 → 空中按 X 出 aerial_01 追击浮空目标。预期现象：4 段伤害 10/14/18/12（假人 100→46 HP）、假人被上挑浮空约 1.4 秒、落地倒地后自行恢复、左下角连击计数与伤害数字出现、F2 复位后满血重来。

M2 刷怪房间（训练图不动，副本在 `/Game/UEMMO/Maps/L_CombatRoom01`，编辑器 Content Browser 双击后运行，独立包可在快捷方式后加参数 `/Game/UEMMO/Maps/L_CombatRoom01`）：走进激活区（房间中部）即开始一局——出口方块变红表示 Running 锁定；清完两波敌人后出口变绿、走进出口即结算离开。当前试玩包内自动开波尚待后续接线，结果界面与重试用调试命令驱动：按 ` 键打开控制台，输入 `UEMMODebugRoomResult 1`（真实清关 → 金色 **Victory** 界面 + 出口变绿）、`2`（真实失败 → **Defeat** 界面）、`3`（点一次 Retry 等价的真实重试，界面关闭后可继续操作）、`4`（Return 离开房间）；界面按钮也可直接鼠标点击（双击只处理一次）。左上角横幅仍显示 M1 文案（HUD 文字属源码常量，本次任务不改源码）。

## 先读这些

**执行任务先读根目录 [TASKS.md](TASKS.md) 和 [HANDOFF.md](HANDOFF.md)。** 现已将后续工作拆成76项小任务；任务卡在Docs/Tasks，每项可独立验收。每完成1项立即更新总表，每完成3项或停止时交接。由用户试玩的任务单独标记，AI不能代签。

1. [游戏与技术设计](Docs/01-游戏与技术设计.md)：目标、阶段、坐标、输入、连招、判定、浮空、敌人、房间、装备、存档、架构、资源和验证。
2. [准备与环境操作手册](Docs/02-准备与环境操作手册.md)：安装、路径、工具链、恢复步骤、所有脚本与故障排查。
3. [资源目录与缺口](Docs/03-资源目录与缺口.md)：已入库模型/动作/音频，来源、许可、待补动作。
4. [Agent 工作流](Docs/04-Agent工作流.md)：ZCode/CodeBuddy 接手提示、日常循环、MCP 评估办法。
5. [验证记录](Docs/05-验证记录.md)：实际完成情况、证据和限制。
6. [M1 原阶段计划](Docs/06-M1实现任务.md)：保留设计背景；执行以总表中的小任务卡为准。
7. [AGENTS.md](AGENTS.md)：编程 Agent 的项目约定。

## 日常命令

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckEnvironment.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/AcquireResources.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/PrepareContent.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1 -Render
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Package.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestPackage.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckTaskBoard.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/UpdateTaskStatus.ps1 -Id M1-001 -Status IN_PROGRESS -Owner ZCode-Session -Report Docs/Tasks/Reports/M1-001.md
```

报告位于 `Artifacts`；构建和 Shader 缓存不提交 Git。`.uasset/.umap` 由 Git LFS 管理。

## 重要状态

- 使用兼容的 Build Tools 2022 / MSVC 14.44.35229；旧 14.38 已在 UE 完整编译中失败，14.42 被引擎禁用。
- 官方模板 128 个资产已就位，UE 枚举 97 个动画，Kenney 四个音效已导入。
- Quaternius 免费候选包下载不可达，未冒充已下载；当前可运行基线不依赖它们。
- ZCode 安装不等于 Agent 到 UE 的交互已完成验证；本仓库的命令行闭环已验证。
- 未安装 UE MCP；现有脚本能执行准备、构建、测试和截图。
- 单机逻辑完成后才接现有服务端；现在没有网络代码。

## 下一步

登录 ZCode，打开本目录，让它读取 AGENTS.md、TASKS.md、HANDOFF.md 和 Docs/Tasks/执行与交接规则.md。M1 技术任务（M1-001..M1-039）与 M2 技术任务（M2-001..M2-015）已全部完成；当前唯一待办是 **M2-H01 用户阶段验收**：用户按上面"M2 刷怪房间"说明实际试玩（编辑器或独立包均可，完整步骤见 `Docs/Tasks/Reports/M2-015.md`），确认刷怪房间节奏、敌人数量与连招成长衔接后该任务才能标 DONE。验收前不要越过阶段用户验收去扩展装备成长或联网。
