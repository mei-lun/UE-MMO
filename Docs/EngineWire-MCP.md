# EngineWire MCP

本机安装固定到`v0.6.0-beta-e`，上游提交`6ea45acc2a71cd4d6d680721893c9aff855243fc`，配套npm包同为`0.6.0-beta-e`。该标签的插件目录和UE插件列表显示名为`McpAutomationBridge` / `MCP Automation Bridge`；不要套用dev分支后来的`EngineWire`目录名。上游仓库为https://github.com/ChiR24/Unreal_mcp，MIT许可证保存在本地固定源码中。

## 使用

1. 在仓库根目录运行`rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Run.ps1 -Editor`。
2. 编辑器加载完成后，在此项目新开Codex会话，加载项目配置中的`ue_mmo_enginewire`。旧会话的工具列表不代表新配置已加载。
3. 要求Agent通过`unreal`先`search`，再`describe`，最后`execute`。例如“列出当前关卡Actor”“查看RoomTrigger组件和碰撞属性”“截图当前视口”。
4. 验证连接：`rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestEngineWire.ps1`。它要求编辑器已打开且关卡有Actor；输出新的JSON、日志和PNG，截图仍需实际查看。

配置在本机`.codex/config.toml`，不提交Git；安装脚本保留其他配置，只管理带标记的EngineWire块。监听`127.0.0.1:18090`，stdio服务按`UE_PROJECT_PATH`读取`Saved/MCP/capability-token`。令牌不进入配置、报告或Git。关闭编辑器时工具会返回NOT_CONNECTED。

## 新机器或恢复

```powershell
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/InstallEngineWire.ps1
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Build.ps1
```

Node.js要求20.19及以上，UE C++构建要求项目原有工具链。脚本下载固定标签并核验提交，用版本锁文件安装Node服务。源码和依赖位于被忽略的`.agent-local/enginewire/`，工程以相对AdditionalPluginDirectories引用源码，不复制进引擎目录。首次打开新检出前先安装，避免缺插件。升级必须单独调整版本、重新编译和验收，不直接更新beta浮动标签。

## 操作边界

- 战斗定义以`Data/CombatSystem/*.json`为源，修改源数据后走生成脚本，不只修改生成资产。
- 主要UI由C++构建，应修改其源码；瞬态Widget变化不代表持久化实现。
- 正式资产修改仍需保存、重启重载、构建/测试与本次证据；MCP成功响应不能代替验收。
- 当前是单机原型；插件提供的联机、GAS或素材库操作不代表项目已采用这些系统。
- 同一工程只允许一个编辑器写资产；用户手感和声音签收仍由用户完成。

本次实际安装与验证结论见`Docs/Tasks/Reports/M0-006.md`。
