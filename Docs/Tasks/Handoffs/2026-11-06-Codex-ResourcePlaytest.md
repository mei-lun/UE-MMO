# 单入口最新资源体验包交接（2026-11-06）

- Owner：Codex-20261106-ResourcePlaytest；开始2026-11-06T10:20:00+08:00，交付2026-11-06T10:40:00+08:00；main，基线567deab，实现2fce13c；本轮新增DONE **1项M6-026**。
- 用户要求清除无用入口并生成最新资源体验包。默认启动直接进入公会场景，最新DNFM格斗家与六技能准备好；菜单只保留一个体验入口；F2回复生命/MP并清技能冷却。独立ResourceExperience_存档，领取奖励保存。
- 代码提交2fce13c；固定包2026-11-06T10:35:21+08:00，实际exe SHA256 BBCE3C1FF414E85339A638FC4770F7D9C253DD8AD85123ECE0655C5C6F81B6CD。更新固定Artifacts/Package/Windows，没有新增并行包。
- 验证：rtk proxy powershell.exe调用Build/Test/Test -Render/TestAutomation（UEMMO.Tasks，946/946）/Package/TestPackage全部通过。TestResourceExperience编辑器与-Package分别完成首次、重启两轮，每轮1/1 success=true，12张截图；已查看实际包内startup/depth-front/stomp/menu。7份旧存档仍存在且SHA逐一相同。
- 专项通过角色输入意图接口释放技能，非物理键盘；保存测试经真实波次死亡和领奖。用户签收未代签。
- [完整报告](../Reports/M6-026.md)。原始证据Artifacts/Tasks/M6-026，包内专项2026-11-06T10-36-12+0800-package-experience；回归2026-11-06T10-29-39+0800；package-preservation包含旧存档备份、包身份与日志。
- 首次构建缺少GameFlow头引用已修复并重建；独立只读复查无阻断项。地图缺贴图/地形、Quinn敌人和图标占位保持已知限制，未冒充完整资源替换。
- 下一可领取M6-018；M6-017仍BLOCKED；M1-H01/M5-H03/H04等人工关仍需用户。用户现在可直接双击固定目录UEMMO.exe体验，不需控制台。
- 工作区保留上轮审计未提交HANDOFF改动及原未跟踪审计/历史交接/bake_spine_fighter.py；未覆盖其他工作，不推送。
