# UE-MMO

UE 5.8.3 单机 3D 横版研究工程：固定侧面镜头，支持 X/Y 地面纵深移动，使用现成免费或引擎附带素材，由编程 Agent 完成客户端实现。

**当前版本：M3 技术完成 / 待用户验收（M3-H01）。** 在 M0 房间与移动、M1 战斗技术栈（X 普攻二段、Z 上挑、跳取消后空中追击、伤害/浮空/倒地恢复、训练假人与会话复位、调试面板 F1）、M2 近战敌人与刷怪房间会话（激活区进房、两波刷怪、死亡重试、中途退出清理、胜利/失败结果界面与出口红/绿）之外，M3 已实现单机成长循环技术链：结算奖励领取（胜利界面 Claim 按钮，XP+物品原子入档并提交存档）、30 格背包界面（图标/标签、选中、穿脱按钮）、装备槽位绑定与属性加成实时生效（训练剑 Attack+5，攻击伤害实测 +5）、版本化存档（A/B 双槽+索引提交、启动自动加载、损坏槽自动回退）、单图选择菜单控件与进出副本状态机（菜单 HUD 呈现接线属后续任务）。M3 全部 22 项技术任务已通过自动验证（480 条自动化测试全绿、成长循环完整场景回归、Development 独立包内验证：包内全量自动化、包内成长循环事件 JSON、包内背包/结算界面截图、删除开发 JSON 后包照常运行）。按项目规则，M3 阶段验收（M3-H01）必须由用户实际试玩后确认，任何 AI 不得代签。注意：单图选择菜单的 HUD 呈现与游戏内自动开波仍属后续接线任务（见下面试玩说明）；本包仍是单机原型，没有联网与服务器。

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

M3 成长循环（接在 M2 进房/清怪之后）：

1. **结算领奖**：胜利界面新增 **Claim** 按钮，是真实领取路径——点击后 XP+50、奖励物品原子写入角色背包并立即提交存档（A/B 双槽 + 索引，防断电重复发放）；快速双击只处理一次。
2. **背包与穿脱**：按 ` 打开控制台输入 `UEMMODebugInventory 1` 打开背包界面（`UEMMODebugInventory 0` 关闭），列出的就是当前角色真实背包（图标为占位：红=WPN、绿=ARM、灰白=ACC，见下面许可清单）；点选一行后 **Equip/Unequip** 按钮走真实穿戴路径，属性加成立即生效（训练剑 Attack+5，之后每次普攻多扣 5 血）。无存档时该调试通道会先铸造本地档并放入三件初始物品（仅调试脚手架；正式获取物品走 Claim 领取）。
3. **保存与重启**：存档提交点在结算领取（原子提交，一次写入背包、装备绑定、等级/XP、未领奖励草稿）；存档文件为 `Profile_A`/`Profile_B`/`Profile_Index`，位于引擎存档目录的 `SaveGames` 文件夹（编辑器内是工程 `Saved/SaveGames`；独立包是包目录下 `UEMMO/Saved/SaveGames`）。重启包后自动加载活动槽；活动槽损坏自动回退另一槽，两槽全坏则拒绝加载并保留损坏文件作证据（不会拿到静默生成的空档）。
4. **重启后的加成重挂**：恢复的装备绑定不会自动重新施加到角色（派生属性按设计不入档；自动重挂属后续接线），在背包界面先卸下再穿上即立即生效。
5. **单图选择菜单**：菜单控件（单行地图 + 进入按钮 + 失败重试）已实现并有全量测试，但尚未接到游戏内菜单 HUD（呈现接线属后续任务）；当前进副本仍用上面的地图参数或编辑器打开 `L_CombatRoom01`。

### 占位与资源许可

| 资源 | 来源与许可 | 状态 |
|---|---|---|
| 角色模型/动画 | Epic 官方模板资产（Characters/Mannequins，保留原路径）；Unreal Engine EULA，随引擎安装使用，不再分发 | 在用 |
| 打击音效 | Kenney Impact Sounds，CC0（SourceAssets 保留原 ZIP 与许可文件） | 在用 |
| 装备图标三枚 | UE 引擎内置 AICON-Red / AICON-Green / GradientTexture0；EULA；占位方案（正式图标待 M3-H01 反馈后替换；登记见 `SourceAssets/manifest.json` 的 `engine_builtin_ui_icons` 与 Docs/03 第 5.1 节） | 占位 |
| 训练物品定义/掉落表 | 代码内置替身（HUD staging 常量）；开发机源定义在 `Data/*.json`，不随包分发，包内由代码替身与 cooked 资产承担 | 占位 |
| HUD 横幅文案 | 源码常量（仍是 M1 文案） | 占位 |
| Quaternius 动作候选包 | itch.io 下载不可达，未下载未入库；当前基线不依赖 | 未获取 |

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

### 数据驱动系统规划（讨论阶段）

新增规划入口：[M5 数据驱动战斗扩展总计划](Docs/Tasks/M5-数据驱动战斗扩展总计划.md)。包含系统设计、公共契约、59 项技术/流程任务与 4 项人工验收卡；覆盖配置资产、统一伤害/受击、武器/子弹、单座地面载具、存档迁移和只改配置新增内容的实证。M5 尚未实施，任务暂未登记到总表；M4-000 的服务端接入范围保持原定义。正式执行先做 M5-000 的任务板兼容接入。

登录 ZCode，打开本目录，让它读取 AGENTS.md、TASKS.md、HANDOFF.md 和 Docs/Tasks/执行与交接规则.md。M1 技术任务（M1-001..M1-039）、M2 技术任务（M2-001..M2-015）与 M3 技术任务（M3-001..M3-022）已全部完成；当前唯一待办是 **M3-H01 用户阶段验收**（M2-H01 仍待用户抽空反馈）：用户按上面"M2 刷怪房间 + M3 成长循环"说明实际试玩（编辑器或独立包均可，完整步骤见 `Docs/Tasks/Reports/M3-022.md` 的 M3-H01 验收清单），确认选图进房、清怪、结算领奖、背包穿脱、保存重启与存档恢复的完整成长循环后该任务才能标 DONE。验收前不开始 M4 联网实现（M4 接口采集要求清单已在 M3-022 报告列出，仅采集不实现）。
