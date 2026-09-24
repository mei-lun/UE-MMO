# UE-MMO

UE 5.8.3 单机 3D 横版研究工程：固定侧面镜头，支持 X/Y 地面纵深移动，使用现成免费或引擎附带素材，由编程 Agent 完成客户端实现。

**当前版本：M0 开发与资源基线。** 已有可运行房间、Manny 玩家、Quinn 资源预览、左右/纵深移动、跳跃、复位和自动验证。普攻、上挑、浮空追击、怪物战斗、装备和联网是后续设计目标，目前没有实现。

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

按键：A/D 左右，W/S 纵深，Space 跳跃，R 回到出生点。当前 Quinn 是素材预览，不会受击或攻击。

## 先读这些

1. [游戏与技术设计](Docs/01-游戏与技术设计.md)：目标、阶段、坐标、输入、连招、判定、浮空、敌人、房间、装备、存档、架构、资源和验证。
2. [准备与环境操作手册](Docs/02-准备与环境操作手册.md)：安装、路径、工具链、恢复步骤、所有脚本与故障排查。
3. [资源目录与缺口](Docs/03-资源目录与缺口.md)：已入库模型/动作/音频，来源、许可、待补动作。
4. [Agent 工作流](Docs/04-Agent工作流.md)：ZCode/CodeBuddy 接手提示、日常循环、MCP 评估办法。
5. [验证记录](Docs/05-验证记录.md)：实际完成情况、证据和限制。
6. [M1 实现任务](Docs/06-M1实现任务.md)：下一阶段按依赖排列的 6 个任务。
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

登录 ZCode，打开本目录，让它读取 AGENTS.md 和 Docs/06-M1实现任务.md，从任务 1 的输入缓存和时间轴开始。先不要同时扩展地图、职业或装备系统。
