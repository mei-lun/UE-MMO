# 最近一轮交接：M5-063可见弹头实体完成，v12待试玩

- Owner：Codex-20261017-VisibleBullet；2026-10-17T18:01:47+08:00开始，18:40:25+08:00完成技术验证。
- main；起点da4eff9，实现`c08edd8e51198a6740edd82744d236edbcfabdea`，验收补强`9df5698209e7166a1ca5f1e4c25a1e264b5de983`。本记录所在提交为报告/状态提交。
- 本轮新增DONE **1项：M5-063**；M5-H02 REVIEW、不代签。总表151/175 DONE（M5 51/72），无IN_PROGRESS。
- 详细报告：`Docs/Tasks/Reports/M5-063.md`。TASKS.md仍是唯一任务状态来源。

## 实际修改

用户明确要求“我需要的是一个子弹实体，不是一条线，要能够看到实体的飞行”。v11短拖尾方案不满足要求，反馈留存，停止推荐v11。

删除ProjectileFlightTrail类及生产生成/维护路径、材质生成脚本。弹头改为24×12×12cm不透明受光橙色实体，普通弹/追踪弹1200cm/s、穿透1600、抛物线2000、爆炸1400；寿命和运动策略保留。实际镜头关闭运动模糊，实体碰撞后才提交伤害，命中/超时/复位/EndPlay/LeaveRoom清理。UE API生成M_BulletSolid及31份数据资产，独立进程重载通过。旧M_Bullet/M_BulletTrail历史资产未手改。

八武器验收逐个走真实菜单切图、背包选装备、视口按键进房/发射。同一GUID三次采样间隔至少45ms，目标每次仍未受伤；之后要求扣血和实体清理。最初120Hz首图在网格显示前被请求，实际查看后修正为等待40ms且WasRecentlyRendered，再重建包并复测；不能只凭JSON或截图存在宣称可见。

## 验证结果

所有命令使用`rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File`前缀。

| 检查 | 结果 |
|---|---|
| Build.ps1 | 完整Editor构建Succeeded；补丁后重建成功 |
| UE create_bullet_assets.py + 新进程-UEMMOBulletVerify | 生成/保存/重载success=true |
| TestAutomation.ps1 -Filter UEMMO.Tasks -TaskId M5-063 | **783/783**，success=true；删除两项已退休尾迹用例，保留离场用例 |
| TestWeaponGameplay.ps1 -Editor -TaskId M5-063 -WeaponIds weapon_hitscan_sample | 默认枪60/120Hz各1/1 |
| Package.ps1 -PackageRoot D:\Github-Poj\UE-MMO\Artifacts\Package\M5B-2026-10-17-v12 | 最终Game build/cook/stage/archive，BUILD SUCCESSFUL，exit0 |
| TestPackage.ps1 -IncludeWeaponScenario -PackageRoot 同上 | 冒烟success/rendering=true、局部4/4、布景1/1、实体2/2、离场1/1、真实UI八武器**8/8** |
| TestWeaponGameplay.ps1 -PackageRoot 同上 -TaskId M5-063 -SimulationFps 60 -WeaponIds weapon_hitscan_sample | 60Hz包内1/1，实测1200cm/s |
| Test.ps1 -Render | success/rendering=true，图已查看 |

证据汇总：`Artifacts/Tasks/M5-063/2026-10-17T18-01-47+0800-delivery/verification-summary.json`，success=true。全量`2026-10-17T18-23-34+0800`；包内八武器`2026-10-17T18-37-20-701+0800-gameplay`；60Hz`2026-10-17T18-38-34-642+0800-gameplay`。同一弹头三帧对照`same-bullet-flight.png`是原图裁剪排列，无补画/插帧。

七件远程各三帧已逐项查看；默认枪t0.883/0.933/0.983秒，X-169.3/-109.3/-49.3cm，目标保持100血；后来碰撞降到90并销毁。点射3实体，散弹5实体；首次掉血：散弹70、爆炸67、剑85，其余90。默认命中/清理及60Hz三帧已看。

## 交付、限制与接续

v12：`Artifacts/Package/M5B-2026-10-17-v12/Windows/UEMMO/Binaries/Win64/UEMMO.exe`，SHA256 `CC1D14C5E037C6B4C9F16C1483887C7DB1CD4876D9E1A83873EC1006F783BA65`。包内附试玩说明。

上方Enter→需要换枪先I选行/Equip/Close→向右进房→Q，剑X。备弹0且无补给，T拒绝，不宣称正向换弹通过；弹药不持久、Running禁换装仍保留。固定60/120Hz、自动视口输入不代替真实键鼠/手感/声音的用户签收。

资产ConfigRevision84057a17bd92021e，包内运行07fed29fe3390afe。只读复核确认八份JSON逐字节一致；既有摘要函数直接哈希FName显示文字，normal/Normal差异可复算这两个摘要。记录为独立P2：后续规范化名称/编码并补跨Editor/Game测试；本次不扩展修改摘要算法，当前武器运行无拒绝。请求代码复核无剩余Critical/Important。

保留`Scripts/InstallEngineWire.ps1`原未提交修改（M0-006仍PAUSED）；未推送远程。M1-H01 BLOCKED、M4 DEFERRED。下一步：用户M5-H02复测v12；独立可领取M5-050。若另做摘要一致性修复应先建独立任务卡，按总表领取。
