# 地图与玩家显示修复交接（2026-11-04）

- Owner：Codex-20261104-VisualFix；本轮新增 DONE **2 项：M6-015、M6-013**。
- main 分支；起点 `26a625317bd44ab00caf03a2140f40931350350d`。人物实现 `ad0f8b4a73582231b1ab9c5d613a96333993249f`，状态记录 `4985cb8`；地图实现 `9f31fd97a370e9f74324c4016869116397513a8b`。本机提交，未推送。
- TASKS 唯一状态：205/210 DONE，0 IN_PROGRESS；M1-H01 BLOCKED、M5-H03 REVIEW、M5-H04 TODO、M7-005 BLOCKED、M4-000 DEFERRED 保持原人工/外部边界。

## 实际修复

1. 原版2D玩家平面由pitch=90改为roll=90，修复与Y轴侧视镜头边对边、角色消失的问题；Frame取整，修复图集连续滑动。保留3D回退与隐藏骨架的战斗机制。
2. 菜单顶部主要按钮明确为 **Play GuildagitScene**，进入新公会图；旧训练场与两战斗房仍可逐行选择。
3. UE编辑器API保存唯一PlayerStart=(-400,620,100)及单一主方向光；修复出生点在大触发区内导致初始重叠丢失，触发区左边界改为-350，向右走入即启动既有1波2敌人。
4. 增加独立进程 `Scripts/TestDNFMVisual.ps1` 与 `UEMMO.Acceptance.M6_015.PlayerVisual`，覆盖实际UButton绑定、OpenLevel、地图景物/出生点、精灵可见性/帧/横向纵深/左右翻面、死亡清波、胜利及菜单视口恢复。未伪称真实鼠标/键盘输入；清波用真实Health扣血死亡链的验收布置。

## 验证和开发包

所有命令通过 `rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File ...` 在根目录运行。

- Build.ps1：完整编译成功。
- TestAutomation.ps1 -Filter UEMMO.Tasks -TaskId M6-013：**919/919、0失败、success=true**，证据 `Artifacts/Tasks/M6-013/2026-11-04T11-00-52+0800`。
- Test.ps1 -Render：success=true，X421.657/Y-463.897/跳高130.746cm。
- Package.ps1：BUILD SUCCESSFUL、exit=0；唯一固定目录 `Artifacts/Package/Windows`，生成 **2026-11-04T11:05:40.7633733+08:00**，源码9f31fd9。
- TestDNFMVisual.ps1 -Package -RoomId room_guildagit_01 -TaskId M6-013：1/1成功，证据 `Artifacts/Tasks/M6-013/2026-11-04T11-06-16+0800-package-visual`；实际查看包内新场景/格斗家/翻面/返回菜单图。
- TestDNFMVisual.ps1 -Package -RoomId room_training_01 -TaskId M6-015：1/1成功，证据 `Artifacts/Tasks/M6-015/2026-11-04T11-06-36+0800-package-visual`；旧训练场也可见新玩家。
- TestPackage.ps1 -TaskId M6-013：success=true；X421.861/Y-463.973/跳高130.744cm。
- exe SHA256 **F489478B3C6301E10FA3AAE23FCC66D3C485AC5123639F3295B23C9CD53748C0**，已重算与清单一致；7个既有包内存档逐字节不变。
- 汇总与备份：`Artifacts/Tasks/M6-013/2026-11-04T10-59-23+0800-delivery/`，delivery-summary.json success=true。build-workspace.json 更新为当前代码与本机忽略资产哈希，避免旧工作区身份误导。
- 独立只读复查发现的翻面/返回/胜利/Widget可见性验收缺口均修复；旧失败证据保留，未改写成成功。

## 限制与下一步

修复的是“进入新地图、人物确实显示”的功能问题。**原版地形缺失、灰色缺图材质、平面卡片景物、敌人仍为Quinn、玩家仅待机动画**仍然存在；没有假装恢复完整原版外观或战斗动作。地图前景卡片会遮挡角色局部，属于当前灰色占位场景表现边界。用户试玩手感/声音未代签。

下一步用户点顶部 Play GuildagitScene 体验；如继续资源完善，应补缺失贴图/地形与战斗动作来源，单独登记后续内容卡。第二职业继续遵守M7-005资源门；M1-H01/M5-H03/M5-H04由用户明确验收。

工作区未被reset/clean；本次接管的玩家与地图改动已独立提交。历史审计/滚动交接与bake_spine_fighter.py等此前未提交文件保留原样；DNFM原版uasset本机忽略保存，不上传。
