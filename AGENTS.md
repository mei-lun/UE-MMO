# UE-MMO Agent 工作约定

本项目是 UE 5.8.3 的单机 3D 横版学习原型，必须支持地面纵深走位。先读 README.md、TASKS.md、HANDOFF.md、Docs/Tasks/执行与交接规则.md，再读对应任务卡与设计。

## 任务领取和强制进度记录

- **TASKS.md 是唯一任务状态来源。每完成 1 项立即更新总表和该任务报告；每完成 3 项写一次批次交接，不足 3 项退出同样要写。** 默认一轮最多3项，用户明确连续执行更多时每3项存检查点后继续。
- 一次只领取依赖全部DONE的一项，将状态、Owner、带时区时间、报告链接填完整；同一目录最多一个IN_PROGRESS。
- 任务细则见 Docs/Tasks/<ID>.md；接口见 Docs/Tasks/接口约定.md。旧 Docs/06 六个大任务不再用于执行计数。
- 未测试、失败、部分实现不能标DONE。需用户试玩的H01由用户明确反馈后才能标DONE，AI不得代签。
- 当前会话完成数只统计新增DONE的ID，不把M0既有基线或拆分父任务重复计数。
- 每项报告放 Docs/Tasks/Reports/<ID>.md，记录实际代码提交、验收命令、结果和限制；原始证据按时间戳放 Artifacts/Tasks/<ID>。
- 完成实现提交后，在报告记录其SHA，再提交总表和报告，避免填写未来提交号。当前分支未集成的外部分支成果不解除依赖。
- 每批更新HANDOFF.md并保存历史交接；暂停时设PAUSED/BLOCKED，写明剩余步骤。不要留无交接的IN_PROGRESS。
- 开始和结束执行 Scripts/CheckTaskBoard.ps1。它只检查结构，不代表游戏逻辑已通过。
- 推荐用 `Scripts/UpdateTaskStatus.ps1 -Id <ID> -Status IN_PROGRESS -Owner <工具会话> -Report Docs/Tasks/Reports/<ID>.md` 更新状态；DONE 前先把报告写完并记录已集成的提交 SHA。

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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckTaskBoard.ps1
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

每次交付写明本轮完成数量与具体任务ID、实际修改、验证命令、结果文件、未通过项和下一任务。逐项更新TASKS.md和任务报告，每3项或退出时更新HANDOFF.md；里程碑验证更新Docs/05-验证记录.md。保持设计目标和当前实现状态分开。
