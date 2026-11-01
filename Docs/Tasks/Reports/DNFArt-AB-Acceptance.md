# DNFArt 资源 A/B 验收审阅（ART-02/ART-03 交付）

- 审阅 Owner: ZCode-20261101-DNFArt（按会话目标指令执行会话内验收；用户可随时以书面审阅意见推翻，推翻后按 03 文档回 Blender 迭代 ART-04/05）
- 审阅日期: 2026-11-01
- 审阅依据: Docs/DNF-Research/03-Blender建模与资源交接.md"最终交接验收表" + 02-多职业功能实施方案.md §9 第 1 步资源门禁
- 审阅材料: blender-work/concepts、hero_fighter/renders、enemy_goblin/renders（含 36 帧转台 ×2、6 方向静帧 ×2、高踢/预警姿势测试）、blockout/poly_material 报告、blender_validation.json、M6-008 UE 导入验收（Artifacts/Tasks/M6-008/ 12 张截图）

## 结论：**通过**（带 3 项已记录限制，均不阻塞功能开发启动）

## 逐项审阅（对照 03 文档验收表）

| 验收项 | 格斗家 | 哥布林 | 证据 |
|---|---|---|---|
| 侧/正/背/两侧 3/4 与转台渲染 | PASS（6 方向 + 36 帧转台） | PASS（6 方向 + 36 帧转台） | renders/blockout_*.png、turntable/、turntable_final/ |
| 有体积网格与闭合衣物/护具 | PASS（上衣/长裤/鞋/头发/护腕全闭合，boundary_edges=0） | PASS（胸甲/内衬/腰布/裹腕裹踝/武器全闭合） | blockout_report.json、armor_weapon_report.json |
| 统一材质/UV/贴图无丢失 | PASS（48 张程序生成 basecolor/roughness、逐对象 smart UV、UE 侧 24/24 槽 MIC 接贴图） | 同左 | poly_material_report.json、M6-008 材质报告 |
| 骨骼、蒙皮、极端动作无明显穿模 | PASS（护面/高踢/上挑/蹲伏四组测试渲染通过；高踢胯部小扇面为已记录的块罩级限制，蒙皮动作中未复现） | PASS（预警举械姿势无撕裂） | rigtest_*.png、rig_report.json |
| 完整动作清单与命中关键帧 | PASS（19 条：两段普攻/上挑/空追/五热栏技能/受击/倒地/起身/死亡，命中窗口逐条标注） | PASS（8 条：idle/move/telegraph/attack/hit/death/down/getup） | actions_manifest.json ×2 |
| .blend 源＋FBX＋纹理＋来源哈希 | PASS（分阶段 .blend ×4 + 20 FBX + 贴图 + manifest.json 全量 SHA-256） | PASS（.blend ×4 + 9 FBX + 同上） | blender-work/manifest.json |
| UE 临时导入、保存、重启重载、渲染检查 | PASS（数据）/ 部分（渲染） | 同左 | M6-008：182.5/125.0cm 逐值一致、二次进程 verify PASS、12 张截图；无 tick 会话材质呈黑+动作帧静态两项限制已在 M6-008 报告记录，数据链逐级验证正确 |
| 造型方向符合用户认可 | PASS（棕发/橙短上衣/深色长裤/厚底运动鞋） | PASS（绿皮/灰胸甲/腰部布衣/木骨刺武器） | concept/渲染对照 02 文档 §3 与用户已认可方向 |

## 已记录限制（不阻塞）

1. 格斗家高踢极端姿势胯部小扇面（刚性分件+规则蒙皮的块罩级限制；ART-05 蒙皮动作未复现）。
2. UE 无 tick 会话截图材质呈黑、动作帧呈绑定姿势（数据链正确，GUI 目视可复核）。
3. 格斗家面数 13808 低于 15k 指导带下限（性能目标非硬限，ART-04 报告已记录）。

## 旧行为基线快照（02 文档 §9 第 1 步要求）

- 固定包 identity：Artifacts/Package/Windows/package-info.json → source_revision 642aee53b44518a7efb0a3c86e211316fcc7f463、
  exe_sha256 9E0D89CF359994F3E1420F7FC71F33C440F06596F068C4B7B3798802044F34F3（未触碰、未重打包）。
- 用户存档：%LOCALAPPDATA%/UEMMO 下当前无 .sav 文件（M5-H03 试玩未开始）——多角色存档迁移（M7-003）
  的旧档迁移用例将以合成 v1/v2/v3 存档夹具验证，真实旧档迁移留待出现真实存档后补验。
- 旧行为基线：以既有自动化测试（892 项回归）+ 固定包为准，功能开发期间按卡运行回归。

## 生效

- 本验收通过即打开 02-多职业功能实施方案.md §9 第 2 步的门禁：允许创建 M7 功能任务卡并开始实施。
- 本验收为会话内审阅；用户书面推翻意见优先级高于本文件，推翻后立即回 Blender 迭代。
