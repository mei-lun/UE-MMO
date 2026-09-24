# UE-MMO Agent 工作约定

本项目是 UE 5.8.3 的单机 3D 横版学习原型，必须支持地面纵深走位。先读 README.md、Docs/01-游戏与技术设计.md、Docs/05-验证记录.md 和当前任务计划。

## 范围

- 当前基线 M0：移动、跳跃、固定镜头、地图和资源。不要把 M1 战斗设计报告为已实现。
- 客户端全部实现由编程 Agent 完成；用户负责试玩、玩法决策和未来服务端接口。
- 不实现多人、在线账号或新服务端，除非用户随后明确要求。
- 素材使用现成免费/引擎附带资源。禁止生成角色美术、使用盗版提取资源或自行采购。

## 命令

Windows 原生 PowerShell，在仓库根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckEnvironment.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/AcquireResources.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/PrepareContent.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Test.ps1 -Render
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Run.ps1
```

用户全局指令要求使用 RTK 时，为上述 shell 命令加 `rtk proxy` 前缀。不要让 RTK 压缩掩盖退出码；检查最终 JSON 和 UE 日志。

## UE 规则

- 工程 `UEMMO.uproject`；C++ 模块 `UEMMO`；源码符号不能使用带连字符的目录名。
- X 横向、Y 纵深、Z 高度；不要启用锁住 Y 的平面约束。
- UE Python `Rotator` 使用关键字 pitch/yaw/roll，不用位置参数猜顺序。
- C++/配置直接编辑；uasset/umap 通过 UE 编辑器 API 操作，不写手造二进制。
- 编辑器脚本在 UE 中执行；普通 Python 不能 import unreal。
- 修改资源后保存、重启重载验证；动画播放的瞬态状态不等于已保存资产属性。
- 不同时启动两个写同一工程资源的编辑器进程。
- 新资源放 Content/UEMMO；第三方保持来源隔离，不重命名模板路径破坏引用。
- C++ 修改用完整构建验证，不把 Live Coding 成功当成干净构建。
- 不修改引擎源码来绕过编译器错误；使用受支持工具链。

## 验证

- 修改源码：Build；修改输入/运动：Test；修改镜头/材质/模型：Test -Render 并实际查看图片。
- 命令 exit=0 之外，还必须存在本次生成的 JSON 报告且 success=true。
- 自动运动测试不覆盖真实键盘事件；不要把它描述成已完成手动试玩。
- Artifacts 保存日志/截图/报告，不提交缓存、密钥、原始私人协议或模型服务凭据。
- 补丁只改当前任务。出现失败先读取日志、确认已发生的变更，再修复。
- MCP 可选。未实测的 MCP 不能写成已安装、官方或兼容 5.8.3。
- 不自动推送到远程或发布第三方素材；本机 Git 提交用于恢复。

## 交接

每次交付写明实际修改、验证命令、结果文件、未通过项和下一任务。更新 Docs/05-验证记录.md，保持设计目标和当前实现状态分开。
