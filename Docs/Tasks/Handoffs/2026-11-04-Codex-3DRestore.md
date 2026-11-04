# 恢复3D角色及纠正资源完成口径（2026-11-04）

- Owner：Codex-20261104-3DRestore；起点main `a429f4d`。本轮新增DONE **1项：M6-016**；旧2D方向M6-015改SUPERSEDED，不再计为用户3D需求完成。
- 实现提交 `f3a6b066d9f1c597387cbbb3911f763f06416884`；最后专项断言修正 `161735ead7e09982781e33d9eeeec9cac8b0dce7`。本机提交，未推送。
- 新增明确未完成项：M6-017地图完整外观BLOCKED（缺源贴图/地形）；M6-018完整动作与视觉质量TODO（死亡播放、倒地/起身、逐招质量及用户审阅）。

## 修复与实际结果

恢复已有DNFArt骨骼角色为默认，2D待机图集仅可选预览；修正该模型误沿用Manny的-90度朝向。清理17条动作未键控骨骼的上一姿势残留，统一动作骨架；受击改用hero专用动画，避免Manny受击骨架使身体沉入地下。短攻击/受击片段的默认0.25s过渡缩短为min(0.05s,片段长度×0.15)。

Run.ps1默认经GameFlow直接进入L_DNFM_Guildagit；-Menu保留菜单。HUD显示当前地图名，旧SystemTestRoom按钮标明旧测试房。日志已确认新包2026-11-04T15:44:08+08:00自动进公会图；PID44492，固定目录UEMMO/Binaries/Win64/UEMMO.exe。

**本次恢复的是低精度学习原型3D模型及其跑跳/普攻/受击，不是完整高质量角色或完整地图。** 原版公会纹理、地形仍缺失，灰色卡片景物/占位地面和遮挡仍在；敌人仍用Quinn。专用死亡资源已导入，但运行时死亡仍冻结姿势；没有宣称死亡播放完成。

## 验证

- Build.ps1完整UBT成功；TestAutomation.ps1 -Filter UEMMO.Tasks -TaskId M6-016：919/919、0失败、success=true，证据 `Artifacts/Tasks/M6-016/2026-11-04T13-15-43+0800`。
- 四份新进程资源重载：animation-repair/verify.json=17动作；unify-verify.json=26个sequence/attack montage；reactions-verify.json=2资源；blends-verify.json=11过渡，均success=true。
- Test.ps1 -Render success=true：X422.727/Y-463.891/跳高130.747cm。
- 固定包生成 **2026-11-04T15:05:36.7360250+08:00**，源码161735e；Package.ps1 exit0、BUILD SUCCESSFUL。
- 包内TestDNFMVisual.ps1 -Package -RoomId room_guildagit_01 -TaskId M6-016：`2026-11-04T15-18-16+0800-package-visual`，1/1 success=true。实际查看3D待机/跑/跳/出拳/受击截图，身体保持地上。
- 旧训练场同专项：`2026-11-04T15-18-38+0800-package-visual`，1/1 success=true。
- TestPackage.ps1 success=true：X422.642/Y-463.886/跳高130.745cm。
- 7份既有包内存档SHA256不变；exe SHA256=`2385FFCAC0221FBFFC2BEE64D0C2CA01F220B15EF3918A2C12A02217ED6FF3D3`，与package-info一致。
- 原始日志、旧存档、构建及交付汇总：`Artifacts/Tasks/M6-016/2026-11-04T11-26-57+0800/`；历史失败均保留。独立代码复查提出的验收误报已修正，死亡生命周期缺口明确留到M6-018。

专项触发实际UButton绑定与生产OpenLevel，不能描述为真实物理鼠标；动画隔离拍摄临时停AI并移远，受击直接驱动真实受击入口，清波用Health死亡链，不伪称人工实战。用户未签收美术、手感和音效。

## 下一步与工作区

用户可在已打开的公会图里体验现有3D角色。下一个可执行技术任务为M6-018；M6-017需缺失源资源或用户明确认可的替代美术方向，不能用重复打包代替补齐。M1-H01/M5-H03/M5-H04及M7-005资源门保持原要求。

本轮没有重置/清理他人改动；根HANDOFF原有滚动历史完整保留并前置本摘要。旧审计/交接与bake_spine_fighter.py等此前未提交文件仍在。源码、资产修复与本卡记录独立提交，原版DNFM资产仍本机忽略保存。
