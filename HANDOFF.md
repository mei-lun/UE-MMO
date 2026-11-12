# 最新状态：地图摆位审计+修复拆卡就绪——预备轮，未执行修复，0 项 DONE（2026-11-13）

- **只读审计（无头编辑器 commandlet，10 图全量）证实用户报告**："大量资源位置不对"=火山 44 块岩壁全部悬空+地面渲染浅灰（M_VolcanoRockDark/M_VolcanoFloorBase 名为深色实为浅灰）+岩浆带如斑马线+黑区；工会图地面是占位几何（DNFM_GAGroundPlaceholder_MISSING_GEOMETRY）+84 件装饰悬空+135 件湖对象埋在地下+70 棵前景树遮挡镜头+无背景幕；竹林出口绿板占位+壳体 WorldGridMaterial+地砖同面 z-fighting+幕布不封顶。**关键发现：M8-030 的 PASS 验收截图当时就是现状——摆位正确性从未被验收过。**
- **拆卡就绪**：M8-042（基线入库+CheckMapPlacement 摆位门禁）→043/044（火山摆位/材质）→045/046（工会图地面背景/摆位）→047（竹林收口）→048（出口统一+全绿回归+用户复测），全部 TODO，每轮 3 项。审计全文、Actor 级数据与用户决策点（D1 前景树/D2 星球装饰/D3 岩浆带形态/D4 地面贴图来源，均有默认值）见 [Docs/Audit-2026-11-12.md](Docs/Audit-2026-11-12.md)。
- 本轮性质：审计+文档，**未改任何地图/资源**；原始审计数据在 `Artifacts/Investigation/2026-11-12-map-placement/`（Artifacts 不随 Git 迁移，关键数字已录入审计文档）。
- 下一步：**用户确认 4 个决策点（或按默认）→ 领取 M8-042 开始第一轮（042→043→044）**。
- 报告：暂无（执行轮开始后各卡出报告）。

---

# 最新状态：M8-030 火山黑区填充+动态岩浆 DONE——逐图视觉收口完成，双切片 Success，交付（2026-11-12）

- **M8-030（dress_dark_maps.py）**：火山图+主菜单同构铺装——暗色岩壁背景墙（Cube 板）、中带/前带地面覆盖（棋盘格隐藏）、**双动态岩浆光带**（Time→Frac→Lerp 自发光扫描，材质图自驱动；首版 Sine 图 cook 失败改 Frac 变体+recompile 修复）；训练场遗留蓝灰边界墙/深度标线删除。动态岩浆说明：源数据 11"帧"为单帧重复，无真多帧，以发光脉冲表达。
- **验证（包内）**：`M8_029.VolcanoSlice` **Success**（火山：立绘玩家+双哥布林血条+岩浆带+暗色岩壁，蓝灰残留清除）；`M8_020.BambooSliceCombat` Success；打包 exit=0（LavaPulse cook 失败修复后复烘零失败行）；主菜单截图=暗色大厅+岩浆光带+立绘。
- Owner：ZCode-20261111-A。任务板 valid=True。**用户四轮反馈的修复链全部交付：M8-034（战斗失效）→M8-035（音效）→M8-036（特效）→M8-037（组件化）→M8-038（显示订阅）→M8-039（门控重启用）→M8-040（游玩流程）→M8-041（立绘回归）→M8-030（黑区收口）。**
- 剩余：两个待定技能（用户技能表）；P08 Boss。
- 下一步：**用户复测交付版**（菜单立绘→选图→三图游玩+技能）→ P08 Boss。
- 报告 [M8-030](Docs/Tasks/Reports/M8-030.md)、[M8-041](Docs/Tasks/Reports/M8-041.md)。

---

# 最新状态：M8-041 立绘回归+地图视觉修复 DONE——DNFM 格斗家全屏回归（截图实证），重打包待用户复测（2026-11-12）

- **M8-041（d03526b）**：用户报告"模型换成错误的 3D 模型"定位为 M8-040 的连带回归——M6-021 立绘门 `cvar==1||经验模式`，cvar 默认 0，此前用户全在经验模式故从未暴露；修复=**cvar 默认翻转为 1**（立绘=生产表现，3D 人偶为回退路径）。包内截图实证：主菜单/火山房均显示 DNFM 格斗家立绘（火山切片 Success：立绘玩家+双哥布林血条+岩浆岩壁）。同卡：公开菜单收起 M5-018A 调试房行；火山走带地板换 M_VolcanoFloorBase 深色材质（灰格消失）。
- **验证**：Build exit=0；`M8_029.VolcanoSlice` Success（立绘开）；包内启动 M6-021 回归+菜单立绘截图；打包 exit=0（Nov 12 01:57）。
- Owner：ZCode-20261111-A。任务板 valid=True。
- **剩余**：火山地形完整视觉（黑区填充/动态岩浆）归 M8-030；黑边逐图收口随 M8-030；两个待定技能待用户技能表。
- 下一步：**用户复测**（菜单立绘→选图→游玩；火山/竹林/工会）→ M8-030 → P08 Boss。
- 报告 [M8-041](Docs/Tasks/Reports/M8-041.md)。

---

# 最新状态：M8-040 游玩流程修复 DONE——菜单劫持+火山房可玩性双根因消除，包内双切片 Success（2026-11-12）

- **用户报告"游玩流程有问题"实机诊断**（亲自启动打包版+截图+包内自动化）定位两项根因并修复（24ff6d5）：①M6 经验自动进入把每个正常 boot 直接丢进工会图+隐藏主菜单（交互启动回归正常模式，经验模式仅 `-UEMMOResourceExperience` 显式启用，M6 验收同步显式契约绿）；②火山房三层地图缺陷（触发器未持久化/装饰碰撞挡路/走带无地板）+出生点错位——`fix_volcano_playability.py`+`fix_volcano_player_start.py` 全部持久化修复，切片测试修正为走位启动。
- **验证（包内实跑）**：正常启动零自动进入+菜单展示；`M8_029.VolcanoSlice` **Success**（编辑器+包内双证，结算截图在案）；`M8_020.BambooSliceCombat` **Success**；`M6_026.ResourceExperience` 绿；全量冒烟 exit=0。证据 `Artifacts/Tasks/M8-022/pkgvol-*`、`edslice4/5-*`。
- **教训**：包内自动化是玩法流程唯一可信验收（报告"全绿"必须核对 index.json 的 Success 行——M8-029 的"全绿"证据是被 MSYS 路径转义毁掉的运行，火山房从未可玩）；归档前清残留 UEMMO 进程（锁文件致 UAT 静默保留旧二进制）；UE 路径参数防 MSYS 转义。
- Owner：ZCode-20261111-A。任务板 valid=True。
- 下一步：**用户复测（主菜单→选图→游玩全流程；火山图应能正常开波）** → M8-030 火山动态环境 → P08 Boss。
- 报告 [M8-040](Docs/Tasks/Reports/M8-040.md)。

---

# 最新状态：M8-039 门控重启用（实时事实重推导）DONE——08 方案问题一解决条款全部落地，重打包待用户复测（2026-11-11）

- **M8-039（64b025e）**：`DerivePhase` 抽纯静态函数（UpdateFromFacts 复用同一派生）；`CanStartAttackLive` 在判定现场用当前 facts 重推导（存储相位仅作显示记录，滞后从机制上消除）；`UCombatComponent::BuildPhaseFacts` 单一事实装配（门控与每 tick 推导共用）；`TryStartAttack`+`TryCast` 双门重启用（M8-034 回退项闭环）。修复 CombatComponent.h 缺 CombatStateMachine.h include（FCombatPhaseFacts 完整类型）。
- **验证**：Build exit=0；无 tick 间隔直启断言+实时门 facts 表新断言绿；`UEMMO.Acceptance.M8_031` **3/3**、`UEMMO.Acceptance.M8_038` **1/1**、`UEMMO.Tasks.M1_038` 连招 **4/4**、`UEMMO.Tasks.M1_009` **5/5**、全量冒烟 exit=0。证据 `Artifacts/Tasks/M8-039/`。
- Owner：ZCode-20261111-A。本轮 goal 累计 DONE：**M8-034~M8-039（6 项）**。任务板 valid=True。
- **08 方案剩余**：两个待定技能（用户技能表）；总验收"用户试玩确认六问题逐条消除"随 M8-022。
- 下一步：**重打包 → 用户复测（重点：门控重启用后连招/技能/受击观感）** → M8-030 火山动态环境 → P08 Boss。
- 报告 [M8-039](Docs/Tasks/Reports/M8-039.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：M8-038 显示层订阅相位事件 DONE——问题一解决条款 3 落地，音频/特效链回归不退化，重打包待用户复测（2026-11-11）

- **M8-038（c67138a）**：presenter 订阅 `OnPhaseChanged`（受击相位打断攻击表现/Idle 拆除当 tick 即时重同步，闭合预留的"future interrupt"一轮延迟盲区）；哥布林显示动作=事件决策+每 tick 幂等重应用（解析器只剩保持门/死亡钉/Idle 速度切换）。**事件定动作族，采样定族内位置**。顺带矫正：哥布林受击姿态不再每 2 tick 闪烁（每次受击单一稳定姿态）。
- **验证**：Build exit=0；`UEMMO.Acceptance.M8_038` **1/1**；回归不退化 `UEMMO.Acceptance.M8_031` **3/3** + `UEMMO.Tasks.M1_009` **5/5**（M8-035/036 链）+ 全量冒烟 exit=0。证据 `Artifacts/Tasks/M8-038/`。
- Owner：ZCode-20261111-A。本轮 goal 累计 DONE：**M8-034~M8-038（5 项）**。任务板 valid=True。
- **08 方案剩余**：问题一解决条款 2（门控重启用需先修一帧滞后——另开卡）；两个待定技能（用户技能表）；总验收用户试玩确认随 M8-022。
- 工作区遗留（非本轮，未提交）：三张 umap 编辑器漂移、火山未提交内容（待甄别）。
- 下一步：**重打包 → 用户复测** → 门控重启用卡 / M8-030 火山动态环境 → P08 Boss。
- 报告 [M8-038](Docs/Tasks/Reports/M8-038.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：M8-037 四命名攻击组件化+槽位组件路由 DONE——问题二收口，顺带修绿 M8_031 验收红，重打包待用户复测（2026-11-11）

- **M8-037（25f5b10）**：light_01/light_02/aerial_01 加入 launcher 完成四组件配置（[4][5] 留空待用户技能表）；热吧槽位经 `FindAbilityForSlotAttack` 静态查找走组件路由，未配置槽位直调逐字保留；`TryCast` 语义修正消除**双重起手缺陷**（loadout 在线=经济+交付全走 loadout，组件不重复交付；缺席=组件按 spec 交付）。
- **顺带修复（问题一验收红）**：转换表 CancelWindow 不可达（补 Active/Recovery→CancelWindow、CancelWindow→Recovery 三边）+ 推导吞掉 `[cancel_end, duration)` 尾段（`FCombatPhaseFacts` 增 `CancelEndFrame` 对齐帧表半开窗）。**教训：M8-031 验收带红上线**（报告称绿但验收未单独重跑）——验收命令必须逐条实跑核对 matched/passed。
- **验证**：Build exit=0；`UEMMO.Acceptance.M8_031` **3/3 绿**（含新增 SkillComponentCastAndIsolation：四攻击独立施放+施放后派生 Windup+弹体从组件 spec+隔离+路由查找）；全量冒烟 exit=0。证据 `Artifacts/Tasks/M8-037/`。
- Owner：ZCode-20261111-A。本轮 goal 累计 DONE：**M8-034、M8-035、M8-036、M8-037**（4 项）。
- **08 方案剩余开放项**：问题一收尾（显示层订阅 OnPhaseChanged、门控重启用需先修一帧滞后——另开卡）；"手动：六技能逐一施放形态一致"用户确认随 M8-022；两个待定技能待用户技能表。
- 工作区遗留（非本轮，未提交）：三张 umap 编辑器漂移、火山未提交内容（待甄别）。
- 下一步：**重打包 → 用户复测** → M8-030 火山动态环境 → P08 Boss。
- 报告 [M8-037](Docs/Tasks/Reports/M8-037.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：M8-036 技能特效最小集 DONE——08方案"特效/声音引用全可加载"验收全句闭环，重打包待用户复测（2026-11-11）

- **M8-036（1775027）**：八个 fighter 源图集页（M8-009 exact_texture_path 命中，restore_dnf_atlas 提取、sha 钉档双跑一致）→ `/Game/UEMMO/Combat/FX/` 八纹理+八材质（duplicate M_ThrowStoneVisual 换 PageTex）；四命名攻击行 `cast_fx_*`/`hit_fx_*` 数据引用；新 `AFxFlipbookActor`（一次性播放自毁）由 presenter Started/HitConfirmed 按攻击目录解析生成（launcher 命中=上挑家族原生 RisingUpperEx/stone）。两个待定技能槽位留空待用户技能表。
- **验证**：Build exit=0；导入 8 页全过；PrepareCombat 42 项 FX 校对零失败；`UEMMO.Tasks.M1_009` **5/5 绿**（含 FX 材质路径/可加载断言——与 M8-035 音频半边合成方案验收全句）；全量冒烟 exit=0。证据 `Artifacts/Tasks/M8-036/2026-11-11T02-57-48+0800`（首轮 2 失败=测试 maker 漏配指针垃圾值，已修正+默认空守卫）。
- Owner：ZCode-20261111-A。本轮 goal 累计 DONE：**M8-034、M8-035、M8-036**（3 项）。
- **08 方案剩余开放项**：问题二收尾（light/aerial 组件化+两个待定技能需用户技能表）；问题一收尾（显示层订阅 OnPhaseChanged、门控重启用需先修一帧滞后）；总验收用户试玩确认项随 M8-022。
- 工作区遗留（非本轮，未提交）：三张 umap 编辑器漂移、`Content/UEMMO/DNFM/Volcano/`+`DA_room_volcano_01`+`place_volcano_room_actors.py`（待甄别）。
- 下一步：**重打包 → 用户复测（攻击/技能恢复+打击音+特效+第三hit手感）** → M8-030 火山动态环境 → P08 Boss。
- 报告 [M8-034](Docs/Tasks/Reports/M8-034.md)、[M8-035](Docs/Tasks/Reports/M8-035.md)、[M8-036](Docs/Tasks/Reports/M8-036.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：M8-035 攻击音补接+第三hit节奏 DONE——08方案问题三/四音频缺口收口，重打包待用户复测（2026-11-11）

- **M8-035（22fedb3）**：combat-attacks.json 四玩家攻击行携带 `swing_sound_path`/`hit_sound_path`（方案问题二.1 的数据驱动资产引用口径）；UAttackDefinition 解析；create_combat_assets.py 校验/加载/应用/校对；presenter 追加 Swing 事件类型（预解析非空才提交，敌人行静默形态）+Hit 事件按攻击解析打击音（缺省回退 ft_dmg_01）；launcher hit_stop 0.04→0.05（"第三 hit 加大"）。
- **验证**：Build exit=0；PrepareCombat updated=9 校对零失败（json-vs-assets 音频 8 项对齐）；`TestAutomation -Filter UEMMO.Tasks.M1_009` **5/5 绿**；全量冒烟 exit=0。证据 `Artifacts/Tasks/M8-035/2026-11-11T02-39-21+0800`。
- Owner：ZCode-20261111-A。本轮新增 DONE：**M8-034、M8-035**（2 项）。
- **08 方案剩余开放项**：问题二收尾（light_01/light_02/aerial_01 组件化+两个待定技能需用户确认技能表）；问题四.2 技能特效最小集（M8-036 候选）；问题一收尾（显示层订阅 OnPhaseChanged、门控重启用需先修一帧滞后）；问题五组件数据随问题二。总验收的用户试玩确认项随 M8-022。
- 工作区遗留（非本轮，未提交）：三张 umap 编辑器漂移、`Content/UEMMO/DNFM/Volcano/`+`DA_room_volcano_01`+`place_volcano_room_actors.py`（M8-028/029 已提交过火山内容，此为后续漂移，待甄别）。
- 下一步：**重打包 → 用户复测（攻击/技能恢复 + 每击打击音 + 第三hit更重手感）** → M8-036 技能特效最小集 → M8-030 火山动态环境。
- 报告 [M8-034](Docs/Tasks/Reports/M8-034.md)、[M8-035](Docs/Tasks/Reports/M8-035.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：M8-034 攻击/技能失效回归修复 DONE——相位机冻结根因消除，重打包待用户复测（2026-11-11）

- **回归根因（用户包日志+回归测试双重实证）**：M8-031 相位推导注入只活在 TickCombat 的 Attacking 路径末尾，非 Attacking 分支提前 return——攻击一结束推导永停，相位冻结在最后攻击相位；`CanStartAttack` 门控从此静默拒绝一切攻击/技能启动（用户："攻击完全失效了，技能也没有了"）。技能失效同源（M5 技能是给下一击上增益，攻击起不来增益永不到账）。
- **M8-034 修复（e707f84）**：①两处门控注释停用（Free 状态检查保持唯一门禁，状态机转纯观察层）；②注入抽 `UpdatePhaseMachine()` 全路径推进（TickCombat 顶部+Attacking 尾部）；③teardown `ForcePhase(Idle)` 绕表强制归位+清取消旗标；④两条回归测试护航（`TeardownNeverLocksThePhaseMachine` 修复前精确复现用户症状、`ChainWindowStartAcceptsTheFollowUp` 窗口首帧链式吃键），运行器 `Scripts/TestCombatAttackLifecycle.ps1`。
- **验证**：Build exit=0；新验收 matched=2 passed=2（`Artifacts/Tasks/M8-034/2026-11-11T02-16-24+0800-attack-lifecycle`）；全量冒烟 exit=0。修复前红的复现证据同目录 02-08-56 批次。
- **门控未重启用**：重启用前必须先解决链式实时帧 vs 滞后相位的一帧差（门控读实时事实），另开任务卡。
- Owner：ZCode-20261111-A。本轮新增 DONE：**M8-034**（1 项）。任务板 valid=True，259 项，DONE=247。
- 工作区遗留（非本任务，未提交）：`L_DNFM_Bamboo01/L_CombatRoom01/L_CombatRoom02.umap` 编辑器漂移、`Content/UEMMO/DNFM/Volcano/`+`DA_room_volcano_01`+`place_volcano_room_actors.py`（M8-028/029 已提交过火山内容，此为后续漂移，待甄别）。
- 下一步：**重打包 → 用户复测攻击/技能** → M8-030 火山动态环境 → P08 Boss。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。
- 报告 [M8-034](Docs/Tasks/Reports/M8-034.md)；[M8-031 回归注记](Docs/Tasks/Reports/M8-031.md)已同步。

---

# 最新状态：08 方案质量收口全部完成——第十三版包交付（含攻击音效 SND_* 资产），待用户试玩确认（2026-11-09）
- **M8-028 进行中（4fb4a51）**：volcano map import script 创建（适配竹林管线：tile 空集跳过、44 对象按标定放置、材料从 goblin 母材质 duplicate、空 tile 记录）。剩余：UE Python API 兼容问题（rotation/scale 方法名）需修复后重跑导入 → room_volcano 接线 → 实拍验证。
- **M8-032 补充**：攻击者侧挥击音/打击音（ft_atk_01..03 + lowKick_impact + 1st_strong_lowkick_hit）已从 M8-009 定位清单提取并 sha 钉档（b5f4e2a）；导入 SND_* + 经技能组件 CastSound 接线=下轮。
- **段表覆盖校验**（aaf3a51）：四 legacy 攻击段表全覆盖校验绿——方案文档问题五完成。

- Owner：ZCode-20261107-A。M8 累计 DONE **30**；M8-022 IN_PROGRESS（H 类——第十二版包 READY FOR USER）；M8-025 SUPERSEDED；M8-028~030 TODO（P07 火山挂起待质量确认后恢复）；提交 `141f9c3`→HEAD。
- **08 方案六问题收口状态**：
  - 问题一 状态机 ✅（M8-031 part1 e731cd8，验收 UEMMO.Acceptance.M8_031.StateMachine 绿）
  - 问题二 技能组件化 ✅（M8-031 part2 c3fee6a，USkillAbilityComponent 六技能+launcher 迁出 M5 管线，验收 SkillAbilityTests 绿）
  - 问题三 手感调校 ✅（M8-032 bb808fe，相机 punch+时间微膨胀+受击原声，回归绿）
  - 问题四 美术缺损 ✅（M8-033，四图审计完成、P13 延期明确记录）
  - 问题五 显示段表覆盖 ✅（aaf3a51，四 legacy 攻击段表全覆盖校验绿）
  - 问题六 旧弹道 ✅（M8-031 part2 launcher 迁出 M5 管线）
- **第十二版包**（PKG exit=0）：包含全部修复+存档恢复。全量回归绿。
- 下一步：**用户试玩第十二版包确认六问题消除** → M8-022 收口 → P07 火山恢复（M8-028~030）→ P08 Boss。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。
- 报告 [M8-024](Docs/Tasks/Reports/M8-024.md)～[M8-033](Docs/Tasks/Reports/M8-033.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：第十一版包（质量收口全部包含），READY FOR USER——待用户试玩确认（2026-11-09）

- Owner：ZCode-20261107-A。M8 累计 DONE **30**；M8-022 IN_PROGRESS（H 类——第十一版包 READY FOR USER）；M8-028~030 TODO（P07 火山挂起）；提交 `141f9c3`→HEAD。
- **第十一版包**（PKG exit=0）：包含全部质量收口修复——状态机+技能组件化+窗口化+工会补色+竹林主入口+命中反馈栈。存档恢复 ✓。
- **质量收口状态**：方案文档六问题中一/二/三/四/五的代码实现全部完成；问题六（正式 UI）归 P11。用户试玩确认后 M8-022 收口。
- **P07 火山**：M8-027 资产提取 DONE；M8-028 part1 标定 DONE；M8-028 part2 导入挂起待质量确认后恢复。
- [检查点 8](Docs/Tasks/Handoffs/2026-11-08-ZCode-20261107-A-checkpoint8.md)；报告 [M8-024](Docs/Tasks/Reports/M8-024.md)～[M8-033](Docs/Tasks/Reports/M8-033.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。
- 下一步：**用户试玩第十一版包** → 质量收口确认 → M8-028~030 火山恢复 → P08 Boss。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：质量收口 M8-031/032/033 全部 DONE——第十版包含全部修复，待用户确认（2026-11-09）

- Owner：ZCode-20261107-A。M8 累计 DONE **30**；M8-022 IN_PROGRESS（H 类——第十版包 READY FOR USER）；M8-028~030 TODO（P07 火山挂起）；提交 `141f9c3`→`ae6a662`。
- **质量收口三卡全部 DONE**：
  - M8-031：UCombatStateMachine 十相位状态机+转换表+施放门控（part1 e731cd8）+ USkillAbilityComponent 六技能组件化+launcher 弹体迁出 M5 管线（part2 c3fee6a）
  - M8-032：命中反馈栈（相机 punch+时间微膨胀）+ 受害者原声链确认（bb808fe）
  - M8-033：四图缺损审计（工会补色 ✓/竹林 ✓/训练 ✓/主菜单 ✓）+ P13 延期项明确记录
- **第十版包**：PKG exit=0 → 全部修复包含 → 切片回归绿 → 存档恢复 ✓
- 下一步：**用户试玩第十版包确认六问题逐条消除** → M8-028~030 火山恢复 → P08 Boss。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。
- 报告 [M8-024](Docs/Tasks/Reports/M8-024.md)～[M8-033](Docs/Tasks/Reports/M8-033.md)；方案文档 [08](Docs/DNF-Research/08-战斗质量收口方案.md)。

---

# 最新状态：M8-031 全部完成（part1 状态机 + part2 技能组件化），下一步 M8-032 手感调校（2026-11-09）

- Owner：ZCode-20261107-A。M8 累计 DONE **27**；M8-031 全部完成；M8-032/033 就绪；M8-022 IN_PROGRESS（H 类）；提交 `141f9c3`→`c3fee6a`。
- **M8-031 part1（DONE e731cd8）**：UCombatStateMachine 十相位显式状态机+转换表+相位事件；集成进 UCombatComponent；验收绿。
- **M8-031 part2（DONE c3fee6a）**：USkillAbilityComponent 每技能一个实例（施放条件/弹体规格/命中反馈声音自包含）；AThrowStoneProjectile 泛化（每实例飞行剖面+敌人路径命中模式）；角色携带六技能组件；输入层纯路由（零特判）；launcher 弹体从 M5 管线迁入技能弹道。验收 SkillAbilityTests 绿。
- 下一步：**M8-032 手感调校**（命中反馈栈+逐技能节奏）→ M8-033 美术审计 → 统一重打包交付。
- 用户事项：试玩第九版包。

---

# 最新状态：M8-031 part1 状态机 DONE（验收测试全绿），part2 技能组件化下轮（2026-11-09）

- Owner：ZCode-20261107-A。M8 累计 DONE **27**；M8-022 IN_PROGRESS（H 类）；提交 `141f9c3`→`e731cd8`。
- **M8-031 part1（DONE）**：`UCombatStateMachine`——十相位显式状态机（Idle/Windup/Active/Recovery/CancelWindow/HitStun/LaunchAir/Knockdown/GetUp/Dead）+数据镜像转换表+相位派生（帧计数对攻击窗口）+相位事件；集成进 UCombatComponent（子对象+每 TickCombat 推进+施放门控 CanStartAttack）。验收 `UEMMO.Acceptance.M8_031.StateMachine` 绿（合法/非法转换对、light_01 窗口相位派生、受击覆盖、门控拒绝）。
- **part2（下轮）**：USkillAbilityComponent 六技能首批组件化（施放/帧段/命中盒/弹体/特效/声音自包含；launcher 弹道从 M5 管线迁入）→ M8-032 手感 → M8-033 美术。
- 用户事项：试玩第九版包。

---

# 最新状态：M8-027 火山资产提取 DONE（确定性双跑），下一步 M8-028 导入（2026-11-09）
- **M8-028 全部完成（d60943f+2806369）**：44 对象导入放置+room_volcano 行+DA_room_volcano_01 生成+RoomTrigger/Exit 接线+PrepareRooms 全绿（含 trigger_references_row_asset 确认）。第十版包重建（PKG exit=0、存档恢复）包含火山图。M8-029（碰撞标定+专项）/M8-030（动态环境）下轮。
- **M8-028 部分完成（d60943f）**：volcano import script 创建+44 对象放置成功+room_volcano 行+rooms.json 更新。剩余：PrepareRooms 管线跑通→RoomTrigger/Exit 放置→M8-029 碰撞标定→M8-030 动态环境→专项测试→重打包。
- **M8-028 part 1 DONE（17be80d）**：volcano_calibration.json（s=1.7615、深度方向 y-down→+Y、CONTACT 近景带 608-645px、内容范围 X -491..2093cm）+ placement world 全量回填（44 对象 x/foot_y/宽高 cm）。剩余：import_dnfm_volcano_map.py 适配（300 行级）→ room_volcano 接线 → 实拍。
- **M8-028 执行计划（下轮接续）**：①建 `volcano_calibration.json`（s=1.7615 全局 px 密度沿用；原点=placement px(0,0)；临时走带由 44 个地面对象的 y 带推导，G1 引擎内标定归 M8-029）；②适配 import_dnfm_bamboo_map.py→import_dnfm_volcano_map.py（tile 空集跳过——manifest 的 empty tile def 需守卫；44 对象按层深度放置；DISTANTBACK 远景；spawn/出口按内容）；③rooms.json 加 room_volcano 行→PrepareRooms 管线→触发/出口；④渲染探针实拍对照源预览。

- Owner：ZCode-20261107-A。M8 累计 DONE **27**；M8-022 IN_PROGRESS（H 类）；M8-028 就绪；提交 `141f9c3`→`a81f9bf`。
- **M8-027（DONE）**：火山图 44 对象/64 图帧/66 PNG 提取落盘（`Data/DNFMReplica/volcano_assets/` + placement），**双跑字节级确定性 ✓**；两处火山结构差异（零图扩展 tile、纯逻辑动画对象）按 map_volcano.json 审计口径容忍记录。`build_volcano_assets.py` + `check_volcano_determinism.py`。
- 下一步：**M8-028 火山图导入**（对象式地面适配——tile 空集；DISTANTBACK 全景层；room_volcano 行+触发/出口）→ M8-029 碰撞/专项 → M8-030 动态环境 → P08 Boss。
- 用户事项：试玩第九版包 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`（窗口化+菜单已实证）。

---

# 最新状态：P07 火山图拆卡完成（M8-027~030 四卡），M8-027 就绪待执行（2026-11-09）

- Owner：ZCode-20261107-A。M8 累计 DONE **26**；M8-022 IN_PROGRESS（第九版包 H 类待试玩）；M8-025 SUPERSEDED；**M8-027~030 登记（P07 四卡）**；提交 `141f9c3`→HEAD。
- **P07 拆卡**：M8-027 资产提取与标定输入（适配 M8-011 会话脚本 build_bamboo_assets.py 到火山图；源=restored maps 火山脚本+图集；确定性双跑）→ M8-028 地图导入与全景层（DISTANTBACK 远景层+room_volcano 接线）→ M8-029 碰撞/出生出口与专项 → M8-030 动态环境（28 个 FX 预fab）。
- 源数据盘点：`Data/DNFMReplica/verify/map_volcano.json` 完备（对象分层/地面完整性/标定输入/缺口 G1 通道语义 G2 原点=引擎内标定口径）。
- [检查点 8](Docs/Tasks/Handoffs/2026-11-08-ZCode-20261107-A-checkpoint8.md)；报告 [M8-024](Docs/Tasks/Reports/M8-024.md)～[M8-025](Docs/Tasks/Reports/M8-025.md)。
- 下一步：**M8-027 执行**（火山资产提取双跑）→ M8-028~030 顺序推进。用户事项：试玩第九版包 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：第九版包菜单可见性实证（Map Select 完整渲染），待用户试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **26**；M8-022 IN_PROGRESS（第九版包 READY FOR USER，H 类）；提交 `141f9c3`→`835dae4`。
- **菜单可见性实证**：第九版包内 M8-027 延迟实拍（8 秒）——**Map Select 完整渲染**（主 Enter=竹林、五行目录含 Bamboo01、SystemTestRoom）。此前"无菜单"=早期截图竞态（HighResShot 抢在菜单首帧前）+菜单挂载在开机后约 2-3 秒。
- 全链修复清单（第六~九版包累积）：竹林主入口、工会补色（35/35 贴图、0 占位槽）、窗口化 1280x720 开机强制、菜单指向竹林。
- 下一步：**用户试玩第九版包**（等待 2-3 秒菜单出现→选 Bamboo01/GuildagitScene）→ M8-022 收口 → P07 火山图。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

[检查点 8](Docs/Tasks/Handoffs/2026-11-08-ZCode-20261107-A-checkpoint8.md)

# 最新状态：M8-025 前提核查作废（工会图=DNFM 重制版原貌），下一步 P07 火山图拆卡（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **26**；M8-022 IN_PROGRESS（第八版包 READY FOR USER，H 类）；M8-025 SUPERSEDED；提交 `141f9c3`→`f5f883c`。
- **M8-025 前提核查**：工会图密度差距不成立——源恢复布局仅 1051 个 MeshRenderer（无 2D 层）；UE 地图 1006 网格 Actor 全数在图（X -3000..3000 均匀分布）。"5050 对象"预览=经典 DNF 端游版面，与 DNFM 重制版 3D 工会基地是两个版本——观感差异=版本差异。任务按证据 SUPERSEDED。
- 待办：P07 火山图拆卡（缺项处理/地砖、全景层、碰撞/出生出口、动态环境）→ P08 Boss → P09/P10 双职业。用户事项：试玩第八版包（窗口化 1280x720；竹林/工会图均含）。
- 下一步：P07 火山图拆卡执行。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：M8-026 第二轮——开机强制窗口化已实证，第八版包待用户试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **26**；M8-022 IN_PROGRESS（第八版包 READY FOR USER，H 类）；提交 `141f9c3`→`799a35a`。
- **M8-026 第二轮**：用户"没有看到变化"——根因=包内 Saved/Config 的 GameUserSettings.ini（历次全屏运行写入）**穿越重打包**并覆盖 Default 种子（分辨率生效了、模式没有）。修复=HUD BeginPlay **开机强制 Windowed 1280x720**；第八版包+人为预埋陈旧全屏档验证：开机改写为 FullscreenMode=2+1280x720 ✓。
- M8-024（DONE）：工会补色（35/35 贴图、1012 槽 0 占位）。M8-025（TODO）：工会密度（5050 2D 层对象）。取景终局：M0 高视野（用户选定）。
- 第六/七/八版包均恢复存档；第八版含开机强制+全部既有修复。
- [检查点 7](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint7.md)；报告 [M8-022](Docs/Tasks/Reports/M8-022.md)～[M8-026](Docs/Tasks/Reports/M8-026.md)。
- 下一步：**用户试玩第八版包**（必为 1280x720 窗口）→ M8-025 工会密度 → P07 火山图。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：M8-026 窗口化 1280x720 DONE——第七版包待用户试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **26**；M8-022 IN_PROGRESS（第七版包 READY FOR USER，H 类）；提交 `141f9c3`→`cfd7b5c`。
- **M8-026（DONE）**：用户"窗口不应默认全屏，按原始资源大小适配"——取证=DNFM 源视频资源 tkhd 帧尺寸 **1280x720**（设计分辨率）；`Config/DefaultGameUserSettings.ini` 打包默认窗口化 1280x720；第七版包 boot 日志实证 `r.setres:1280x720`。
- M8-024（DONE）：工会图补色——35/35 缺失贴图提取导入、78 占位材质重建、1012 槽 0 占位（白图→暮色纹理）。
- M8-025（TODO）：工会图视觉密度（5050 个 2D 层对象 vs 已导入 279 网格）——竹林 2D 管线套用。
- [检查点 7](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint7.md)；报告 [M8-022](Docs/Tasks/Reports/M8-022.md)～[M8-026](Docs/Tasks/Reports/M8-026.md)。
- 下一步：**用户试玩第七版包**（窗口化 1280x720 启动）→ M8-025 工会密度 → P07 火山图。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：M8-024 工会图补色 DONE——白图已除，第六版包待用户试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **25**；M8-022 IN_PROGRESS（第六版包 READY FOR USER）；M8-025 TODO（工会图密度）；提交 `141f9c3`→HEAD。
- **M8-024（DONE）**：用户"工会地图还是白的，也需要补齐"——根因=工会图材质早在 M6-013 就引用了 GA_* 贴图资产但**贴图从未导入**（源 cab 未定位）。修复：定位容器=`prefabs_3d_guildagit_textures.npk`（10823 包二进制快扫）→ 提取 35/35 缺失贴图（0 缺失）→ 重建 78 个占位材质 → **地图 1012 材质槽 0 占位**（白图→暮色纹理场景，实拍为证）。
- **M8-025（TODO）**：工会图视觉密度=5050 个 2D 层对象（M7 仅导入 279 网格）——竹林 2D 管线套用，独立卡。
- 取景终局（M8-023）：两轮 DNF 近景标定被否，还原 M0 高视野；保留边界接线+竹林主入口。
- 第六版包：PKG exit=0 → 主目标=竹林 ✓ → 包内切片绿 ✓ → **工会补色贴图 69 条入包** ✓ → 存档恢复 ✓。
- [检查点 7](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint7.md)；报告 [M8-022](Docs/Tasks/Reports/M8-022.md)～[M8-024](Docs/Tasks/Reports/M8-024.md)。
- 下一步：**用户试玩第六版包**（竹林直进+工会图在目录列表）→ M8-025 工会密度 → P07 火山图。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：取景终局——用户选定 M0 高相机视野（已还原），第五版包待试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **24**；M8-022 IN_PROGRESS（第五版包 READY FOR USER）；提交 `141f9c3`→HEAD。
- **取景终局**：两轮 DNF 近景标定（660/FOV55、1050/FOV40）均被否决，用户明确"还原成和以前一样的高相机视野"→ 全部还原 M0（FOV55、臂长1700）。**保留的实质修复**：相机锚点房间边界按图接线（竹林 0..1578，修东行走出镜头）+ 菜单主目的地=竹林。回归绿；切片绿；第五版包内复验绿。
- 教训（记忆已存）：取景属用户体验决策，两轮自定标定被否——此类"体验感"参数应先给对照样张让用户选，勿自行定标。
- 下一步：**用户试玩第五版包**（高视野+竹林直进）→ P07 火山图。用户事项：重试 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：取景第二轮标定——扁平舞台视角（FOV40+臂长1050），第四版包待用户确认（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **24**；M8-022 IN_PROGRESS（第四版包 READY FOR USER）；提交 `141f9c3`→HEAD。
- **取景迭代**：1700（太远，角色 11%）→ 660/FOV55（用户："太近、体验感不对"——广角畸变+强视差）→ **1050/FOV40**（长焦扁平舞台视角：角色约 25-28% 屏高、纵深层同移，截图 `Artifacts/Tasks/M8-020/2026-11-08T18-18-36+0800-slice-combat/slice-settled.png`）。
- 第四版包：PKG exit=0 → 包内切片复测绿（killed=2、Cleared、主目标=竹林）。报告 [M8-023](Docs/Tasks/Reports/M8-023.md)（第二轮标定节）。
- 下一步：**用户四轮试玩确认手感**（仍不合适可继续调：下一旋钮=PivotHeightAboveGround/YFollowRatio/逐图覆写）→ P07 火山图。用户事项：重试 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：M8-023 取景重对齐 DONE——DNF 构图在包内实证，待用户三轮试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **24**；M8-022 IN_PROGRESS（第三版包 READY FOR USER）；提交 `141f9c3`→`636814a`。
- **M8-023（DONE）**：用户"贴图位置都不对"自查定位=游戏内相机取景（M0 臂长 1700 → 世界拉远 2.8 倍，构图整体失真；世界坐标本身正确）。修复：臂长 660（角色约 1/3 屏高+脚下石板地面+竹丛近景——与源构图一致，两轮截图迭代实证）；**相机锚点房间边界首次按图接线**（默认 ±1200 会让玩家在竹林东段走出镜头）；车辆相机断言随新值更新；全量回归绿。
- 第三版包：PKG 5m56s exit=0 → boot 日志主目标=room_bamboo_01 ✓ → 包内切片复测绿（killed=2、Cleared）→ **包内截图实证 DNF 构图**（`Artifacts/Tasks/M8-022/pkg3-verify/slice-settled.png`）。
- [检查点 7](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint7.md)；报告 [M8-022](Docs/Tasks/Reports/M8-022.md)、[M8-023](Docs/Tasks/Reports/M8-023.md)。
- 下一步：**用户三轮试玩**（主 Enter 直进竹林，构图应与原作一致）→ P07 火山图。用户事项：重试 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：M8-022 首轮反馈修复完毕——菜单主入口指向竹林，重包待用户二轮试玩（2026-11-08）

- Owner：ZCode-20261107-A。M8 累计 DONE **23**；M8-022 IN_PROGRESS（第二轮 READY FOR USER）；提交 `141f9c3`→`38e4d30`。
- **首轮用户反馈三问题自查定位**：①菜单主 Enter 被 M6-013 硬编码指向工会图→已修复为主目的地=竹林（新包 boot 日志实证）；②"无贴图"=被送入的工会图的 M7 存量占位材质（公会补色期外）——竹林图在包内贴图完好（包内自动化自截图实证）；③包内切片战斗全链复测绿（hp=43/60、killed=2、Cleared）。
- [检查点 7](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint7.md)+[M8-022 报告](Docs/Tasks/Reports/M8-022.md)（首轮反馈与修复节）。
- 下一步：**用户二轮试玩**（启动 UEMMO.exe→主 Enter 直进竹林）→ P07 火山图。用户事项：重试 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`。

---

# 最新状态：P06 切片就绪——固定包产出+包内战斗全链实证，待用户试玩（2026-11-07）

- Owner：ZCode-20261107-A。M8 累计 DONE **23**（P00～P05 + 06-A/B）；**M8-022 IN_PROGRESS（READY FOR USER，H 类）**；提交 `141f9c3`→`448e776`。
- **M8-020（DONE）**：玩家攻击链竹林清波（Victory 6.9s）；**修复 M2-001 起定义 MaxHP 从未接线的存量缺陷**（哥布林 60 设计血量首次生效，每只恰 6 刀）。
- **M8-021（DONE）**：六条受害者原声（ft_dmg/gbn_dmg）确定性导入 + SetAudioSounds 链首次生产接线；攻击挥击/石击/死亡声留 P13。
- **M8-022（IN_PROGRESS）**：固定包产出（存档先备份后恢复）；**包内自动化冒烟绿**（切片全链在包内成立）；等待用户试玩（H 类验收点在报告）。
- [检查点 7](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint7.md)；报告 [M8-020](Docs/Tasks/Reports/M8-020.md)～[M8-022](Docs/Tasks/Reports/M8-022.md)。
- 下一步：**用户试玩固定包** → P07 火山图。用户事项：试玩 `Artifacts/Package/Windows/UEMMO/Binaries/Win64/UEMMO.exe`（菜单第 5 项 Bamboo01；验收点见 M8-022 报告）。

---

# 最新状态：P05 全清（M8-019A/B/C 三卡 DONE）——竹林生产循环 -game 打通，P06 试玩点就绪（2026-11-07）

- Owner：ZCode-20261107-A。M8 累计 DONE **21**（P00～P05 全清）；任务板 244 项 **235 DONE / TODO 2 / 0 IN_PROGRESS**；提交 `b8071a2`→`eac94f0`。
- **M8-019A（DONE）**："图片不渲染"根因结案——**新建材质 shader 异步编译 ~35s 的截图时序伪影**，渲染管线从未损坏；最小验收全绿（站立绿哥布林+倒地平躺帧）。
- **M8-019B（DONE）**：投掷 AI 接入既有敌脑（LaunchAttack seam，零新状态）；AThrowStoneProjectile（4f×30ms、20³ 全程命中盒、统一 Health 链）；命中/不打/打空三场景专项绿+时序对应表。
- **M8-019C（DONE）**：竹林生产波次全循环 -game 打通——rooms.json 行→DA 管线→图内触发器（**必须放在出生点前方**）→定义驱动出生类（UEnemyDefinition::EnemyActorClass）→击杀计数→MarkCleared→**Victory 结算 HUD 截图**；DA_dungeon_goblin_proto 退役。
- 事故：中途误删哥布林视觉目录，已由脚本重导恢复（可再生，无损失）。
- [检查点 6](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint6.md)；报告 [M8-019A](Docs/Tasks/Reports/M8-019A.md)～[M8-019C](Docs/Tasks/Reports/M8-019C.md)。
- 下一步：**P06 首个可玩切片（用户试玩点）**→ P07 火山 → P08 Boss → P09/P10 双职业。用户事项：可试玩竹林关卡（主菜单目录 room_bamboo_01）。

---

# 最新状态：P05-A1 哥布林实体 PAUSED——图片渲染 UV 二分进行中（2026-11-07）

- Owner：ZCode-20261107-A。M8 累计 DONE **18**（P00～P04 清零）；M8-019 拆为 **M8-019A（PAUSED）/B/C**。
- **M8-019A 已完成部分**：哥布林实体类+显示状态机+定义数据；专项 `UEMMO.Acceptance.M8_019A.GoblinEntity` 断言全绿；946/946 回归绿。**阻塞项：图片平面 -game 不渲染**。
- **根因收敛（三轮 20+ 诊断）**：平面/变换/相机 basis/生成点/材质图/纹理内容全部排除；PageTex 纹理覆盖不生效（渲染始终采样母材质默认页——格斗家页在哥布林 cell 坐标的内容=已解码的 crouch 格斗家 ✓）、UVRect 覆盖生效、test-z（全页 UV）渲染哥布林页成功——**矛盾面=UVRect 数值本身**。下一会话：UV 分量逐项二分（Min.x/Min.y/Size 各改其一测渲染），或临时 Masked→Translucent 验 alpha 采样。
- 报告 [M8-019A](Docs/Tasks/Reports/M8-019A.md)（三轮诊断轨迹+收敛矩阵完整）；提交至 `5e06a56`。
- 下一步：UV 二分 → 修复 → M8-019A 截图验收 → M8-019B/C → P06 切片（用户试玩点）。用户事项：无。

---

# 最新交付：M8 P04 完成 + P05 哥布林实体 PAUSED——累计 18 卡（2026-11-07）

- Owner：ZCode-20261107-A（协调者）。本轮累计 DONE **18 项中的 18**（M8-001～018）；**M8-019A PAUSED**（1 项部分完成）。
- **P04（3 卡）**：三职业离线 manifest（鬼剑士 206 帧/神枪手 282 帧，确定性）→ 双职业 UE 导入+verify 全绿 → 怪物显示资源（哥布林 21 帧+taubeast 82 帧，C++ 契约演进 bDrawFrame，946/946 护航）。
- **P05-A1 哥布林实体 PAUSED**：实体/状态机/死亡链/定义数据全绿（专项 1/1+回归 946/946），**图片平面 -game 不渲染根因未定位**——12+ 轮诊断轨迹完整（全部对象级状态验证正确；唯一渲染成功=瞬移玩家变换+玩家 MID）。**下一会话从报告"未解根因"矛盾面继续。**
- [检查点 5](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint5.md)；报告 [M8-016](Docs/Tasks/Reports/M8-016.md)～[M8-019A](Docs/Tasks/Reports/M8-019A.md)。任务板 244 项 231 DONE、0 IN_PROGRESS、1 PAUSED（M8-019A）。
- 下一步：M8-019A 渲染根因 → M8-019B/C → P06 切片。用户事项：无。

---

# 最新交付：M8 P03 竹林完整地图完成——累计 15 卡 DONE（2026-11-07）

- Owner：ZCode-20261107-A（协调者；UE 写入串行，离线由子代理并行）。本轮累计 DONE **15 项：M8-001～015**；`79ebbae`→`434fe3d`。
- **P03（3 卡）**：雾动画（8 帧 flip_y+染色预烘焙纠正 P02 方向错误；C++ ADNFMPropFrameCycler 按源延时 50s/0.1s 交替；verify+专项 1/1+实拍）→ 出口指示柱（项目适配；走至断言绿；三轮冷进程一致+7 存档 SHA 未变）→ 源对照（3 取样点并排对照逐位实看，无灰板/错误地面/缺层；预览 0.779 缩放映射修正）。
- 竹林图现状：`L_DNFM_Bamboo01` = 4 地砖+46 对象+8 雾帧（4 循环器）+挡墙+出口标记+PlayerStart；专项 `UEMMO.Acceptance.M8_012.BambooSegment` 持续全绿。
- [检查点 4](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint4.md)；报告 [M8-013](Docs/Tasks/Reports/M8-013.md)～[M8-015](Docs/Tasks/Reports/M8-015.md)。任务板 238 项 228 DONE；M8-016（三职业 manifest）运行中。
- 下一步：M8-016 验收 → P04-B/C 登记 → **P05 哥布林敌人** → **P06 首个可玩切片**（用户试玩点）。
- 用户事项：无。

---

# 最新交付：M8 P02 竹林可玩切片完成——累计 12 卡 DONE（2026-11-07）

- Owner：ZCode-20261107-A（协调者；核实/标定/裁图由 7+2 个子代理并行，UE 写入由协调者串行）。本轮累计新增 DONE **12 项：M8-001～M8-012**；`79ebbae`→当前，main 本机提交未推送。
- **P00+P01（9 卡）**：基线保护（946/946 绿+存档/包/源库 SHA256 落档）+ 三职业/两图/两敌/声音特效全量源依赖核实（每卡报告+确定性 verify JSON+协调者实看合成预览；数据在 `Data/DNFMReplica/verify/`）。
- **P02（3 卡）**：竹林像素/世界标定（轴映射 4 代码证据，s=1.7615，63 断言自测）→ `L_DNFM_Bamboo01` 测试图（4 地砖+50 分层对象+碰撞地板+假设挡墙，verify 二进程重载对齐）→ C++ 专项 `UEMMO.Acceptance.M8_012.BambooSegment` **1/1 全绿**：三向墙拦截/跳跃落地/DNFM 2D 视觉/前景遮挡，**A7 裁决 P 读法成立**；全量回归 **946/946**；5 张实机截图协调者逐张实看。
- 关键事实：raw -game 启动落菜单门控态，进竹林图须走 EnterRoom（瞬态 URoomDefinition）；画面四边形必须 NoCollision；A(0=可走)+P(tile top y=40) 实机成立；火山通行语义需另做实机标定。
- [检查点 3](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint3.md)、[批次收官](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-final.md)；报告 [M8-010](Docs/Tasks/Reports/M8-010.md)～[M8-012](Docs/Tasks/Reports/M8-012.md)。任务板 234 项 226 DONE、0 IN_PROGRESS。
- 下一步：登记 **P03**（竹林完整地图：雾动画/动态对象/出入口/保存重载拆卡）→ P04（三职业 manifest）→ P05（哥布林敌人）→ P06（首个可玩切片）。
- 用户事项：无（P02 为测试图切片，未接菜单目录；人工试玩归 P06）。

---

# 上一轮交付：M8 P00+P01 依赖核实全部完成——9 卡 DONE（2026-11-07）

- Owner：ZCode-20261107-A（协调者，7 个并行子代理执行）；本轮新增 DONE **9 项：M8-001～M8-009**；`79ebbae`→`c9c79d2`，main 本机提交未推送。
- P00 基线保护：固定包双 exe SHA256 落档、7 份存档逐 SHA 备份、4 源 SQLite SHA256 冻结 130.5.8.202、开工回归 **946/946 绿**。
- P01 八卡（全程只读源库 mode=ro、零 UE 改动）：格斗家复验+发射锚点/影子/事件字段；鬼剑士 455 动画+9 层衣装+六技能候选；神枪手 16 层零缺帧+左右射击独立帧段+枪口锚点；竹林/火山两图完整依赖（坏账交集全 0，**火山不触发换图**）；哥布林投掷物攻击盒+死亡链；taubeast 三行为窗口+Boss 源配置（牛头巨兽）；声音 21328 WAV 索引、84 剪辑全解析（located 79/keyed 28/missing 6）。
- 每卡验收：报告硬格式+verify JSON 确定性复核+协调者实看合成预览+CheckTaskBoard。数据在 `Data/DNFMReplica/verify/`，证据在 `Artifacts/Tasks/<ID>/2026-11-07T0030+0800/`。
- 已知限制：通行网格 {0,2} 语义需实机标定（P07 前置）；音频未听验；.ai 行为脚本不在资源集（落地按项目适配标注）。
- 下一步：登记 P02（像素/世界标定，入口=map_bamboo.json calibration_inputs）、P04（三职业 manifest 化）、P05（哥布林）执行卡。本轮无游戏内容改动，无人工验收项。
- [批次收官交接](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-final.md)、检查点 [1](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint1.md)、[2](Docs/Tasks/Handoffs/2026-11-07-ZCode-20261107-A-checkpoint2.md)；任务板 231 项 223 DONE、0 IN_PROGRESS。
- 注：本 HANDOFF 提交同时带入上一轮（Codex-20261106-MapFeedback）遗留未提交的"用户反馈核对"段落与其时间线，内容未改动。

---

# 最新交付：DNFM三角色双地图资源替换计划（2026-11-06）

- Owner：Codex-20261106-ResourcePlan；本轮新增DONE **1项M8-000（计划编制），新增游戏功能0项**；main，基线25a0002，文档实现fbe3c9d，未推送。
- [完整计划](Docs/DNF-Research/07-DNFM资源替换与最小复刻计划.md)：默认格斗家/鬼剑士/神枪手、竹林/火山、哥布林投掷兵/taubeast Boss，每角色6技能目标，15个工作包及完整资源替换/验收矩阵。
- 本次核查了downloaded_130.5.8.202/restored补充数据；公会新几何已存在但glTF仍无贴图，不能沿用旧首包缺资源结论，也不能宣称UE原色地图已恢复。两图离线预览与怪物实际帧已查看。
- 只读SQLite与选中怪物图片裁切、15个文档链接检查通过；没有导入资产、修改玩法或重打包。源图/数据保留私有路径，证据Artifacts/Tasks/M8-000/2026-11-06-resource-inventory。
- 下一步先登记P00/P01执行卡，再交付原色地图+格斗家+小怪可玩切片，之后扩第二图/Boss和第二第三角色。不能把资源导出或旧DONE当成新玩法交付。
- [报告](Docs/Tasks/Reports/M8-000.md)、[历史交接](Docs/Tasks/Handoffs/2026-11-06-Codex-ResourcePlan.md)。原工作区审计/地图反馈修改保留，人工验收未代签。

---

# 最新用户反馈核对：地图完整外观仍未交付（2026-11-06）

- 用户实玩认为只有角色替换，地图未见替换。已查实际日志：10:48:11进入的确是L_DNFM_Guildagit，问题是地图内容缺失，不是选错入口。
- M6-012仅摆放场景实例，978/996材质槽缺底图、8处地形/水面等缺几何；“地图替换好了”不能作为当前结论。M6-026没有补这些资源。
- 本轮新增DONE 0项，M6-017仍BLOCKED；[核对与用户原话](Docs/Tasks/Reports/M6-017.md)、[历史交接](Docs/Tasks/Handoffs/2026-11-06-Codex-MapFeedback.md)。原始日志归档Artifacts/Tasks/M6-017/2026-11-06-user-feedback。未改代码或资产、未重打包。

---

# 最新交付：单入口最新资源体验包（2026-11-06）

- Owner：Codex-20261106-ResourcePlaytest；本轮新增 DONE **1项：M6-026**。基线567deab，实现2fce13c；本机提交未推送。
- 双击固定包 UEMMO.exe 即自动进入公会图，默认最新 DNFM 格斗家、六技能可用；菜单仅保留同一体验入口。F2回复生命/MP并清冷却，体验存档独立于旧Profile/Roster/Char槽。
- Build、946/946回归、Test及Test -Render、Package、TestPackage全部通过；默认体验专项在编辑器与包内各完成首次启动和领奖后重启2个进程，各1/1。已实看包内新人物、纵深、技能、单入口菜单截图。
- 固定包生成于2026-11-06T10:35:21+08:00，源码2fce13c，exe SHA256 BBCE3C1F…F81B6CD；实算与清单一致。7份原存档逐SHA核验完好。
- 地图贴图/地形和敌人占位仍未补齐；本轮不是完整美术、多职业或连续地城交付，也没有代签人工试玩。
- [任务报告](Docs/Tasks/Reports/M6-026.md)、[历史交接](Docs/Tasks/Handoffs/2026-11-06-Codex-ResourcePlaytest.md)。原始证据在Artifacts/Tasks/M6-026；包内专项2026-11-06T10-36-12+0800-package-experience。
- 任务板221项、213 DONE、0 IN_PROGRESS；下一可领取M6-018。M6-017仍资源阻塞，用户可先体验本包并反馈手感。

---

# 最新审计：单机技术原型已有成果，完整内容与玩家入口仍未收尾（2026-11-06）

- Owner：Codex-20261106-Audit；本轮新增 DONE **0 项**；main / `567deab`，任务状态与人工签收未改变。
- 总表 **220 项、212 DONE、0 IN_PROGRESS**；完整结论见 [本轮审计](Docs/Audit-2026-11-06.md)，这不是游戏完成百分比。
- 固定包已更新至 2026-11-06T06:26:57+08:00，源码 `9b27808`；本轮实算 exe 哈希与清单一致，到当前 HEAD 只有文档差异。历史原始测试记录 **946/946 Success**；已抽查包内新动作与默认 3D 截图。本轮没有重新构建/运行游戏/打包或人工试玩。
- **M7 玩家流程未闭合**：角色/职业选择 UI 未接，默认角色仍 prototype；六技能验收辅助不等于正常获得流程。连续地城的启动、过图和奖励回调只见测试侧调用，M7-004 的 DONE 不能解释为完整可玩地城已交付，需核定边界并补后续卡。
- 新 DNFM 动作显示已入包但默认关闭；默认 3D 完整动作收尾仍 M6-018 TODO。M6-017 地图缺资源、M7-005 第二职业仍 BLOCKED；普通敌人与装备图标还有占位；M1-H01/M5-H03/H04 未完成用户验收。
- 下一步：登记 M7 生产入口/接线后续卡；当前唯一可领取 TODO 为 M6-018。历史交接：[2026-11-06-Codex-Audit](Docs/Tasks/Handoffs/2026-11-06-Codex-Audit.md)；证据 `Artifacts/Tasks/Audit-20261106/2026-11-06T10-07-12+0800/`。仅新增审计记录，原未跟踪文件保留，未提交或推送。

---

# 上轮交付：DNFM 固定视角角色接入 全链完成（2026-11-06，检查点 2 后续 + M6-025）

- Owner：ZCode-20261106-A（协调者，6 个并行子代理执行）；本轮累计新增 DONE **7 项：M6-019～M6-025**。起止 `9bdea2e`→当前 HEAD，main 本机提交未推送。
- **M6-022 编辑器导入**（`395ce7ff`）：页纹理 Nearest/Clamp/无mip、Unlit+TwoSided+Masked（alpha 直方图证据）、DA_DNFMFighterVisualSet 136 帧全量 0 错配；s=1.7615cm/px；修复材质 SM6 编译与精灵垂直翻转。
- **M6-023 战斗同步**（`09da4cf`）：DNFMCombatVisualPresenter 独立协调类；9 攻击分段映射（bHitPose==active 窗）；事件去重/HitStop/死亡复位不残留。
- **M6-024 状态与技能**（`7a37bbb8`）：9 攻击按真实帧时长校准；受击交替/倒地/起身序列（播完再回）/死亡持末帧/复活复位；跳跃三段状态驱动；12 张实机姿势截图。
- **M6-025 固定包验收**：Build/Package/TestPackage 通过；**全量回归 946/946**；包内公会图/训练场专项通过；exe SHA256 `88A099E3…84C4B`（源码 `9b27808`）；性能 gate0 vs gate1 帧时间持平（2.35~2.39ms）；**7 份用户存档 SHA256 逐一比对完好**。
- **自动验收通过 ≠ 用户试玩通过**。用户事项：① 真实试玩（`Scripts/Run.ps1` 默认进公会图；gate 开关 `-DPCVars=UEMMO.PlayerDNFMVisual=1`，默认 0 保持 3D）；② 决定验收后的默认显示模式；③ M6-018（3D 动作收尾）与本系列并存，不冲突。
- [检查点1](Docs/Tasks/Handoffs/2026-11-06-ZCode-20261106-A-checkpoint1.md)、[检查点2](Docs/Tasks/Handoffs/2026-11-06-ZCode-20261106-A-checkpoint2.md)；报告 [M6-019](Docs/Tasks/Reports/M6-019.md)～[M6-025](Docs/Tasks/Reports/M6-025.md)。人工验收未代签，未推送。

---

# 上轮批次：DNFM 固定视角角色接入——检查点 1/2（2026-11-06）

- Owner：ZCode-20261106-A（协调者，4 个并行子代理执行）；本轮新增 DONE **3 项：M6-019、M6-020、M6-021**。起止 `9bdea2e`→`09da4cf`，main 本机提交未推送。
- 用户目标：按 2026-11-06《UE-MMO固定视角角色接入方案》并行执行资源替换与自动验收（拆卡 M6-019..M6-025）。
- **M6-019**：格斗家四层衣装（body/hair/coat/shoes）136 帧平行性证实，层序 body→shoes→coat→hair；受击/起身/死亡/上挑/五技能 .ani 全部定位；pants0000d 不平行显式缺项。映射 JSON+8 姿势抽查图可复核。
- **M6-020**：`prepare_fighter_visual_set.py` 确定性合成管线；25 动作/136 帧完整衣装图集（单页 2048²），旋转/翻转帧支持并逐帧记录；两遍重跑字节一致；预览经协调者亲自查看。子代理被平台内容过滤器中断于报告前，产出完整性由协调者独立验证（已记录）。
- **M6-021**：`DNFMCharacterVisualSet`+`DNFMCharacterVisualComponent`（脚底锚点/相机 Right-Up/单一翻转/统一比例），`UEMMO.PlayerDNFMVisual` 默认 0 三维路径零回归；新增 8 测试全过，Build/Test 通过。
- **进行中**：M6-023 战斗同步代码+9 测试完成（全量回归 936/936），实现 `09da4cf`，差导入后 -Render 实图证据（board 保持 TODO）；M6-022 编辑器导入为下一步。
- [本轮检查点](Docs/Tasks/Handoffs/2026-11-06-ZCode-20261106-A-checkpoint1.md)；报告：[M6-019](Docs/Tasks/Reports/M6-019.md)、[M6-020](Docs/Tasks/Reports/M6-020.md)、[M6-021](Docs/Tasks/Reports/M6-021.md)、[M6-023](Docs/Tasks/Reports/M6-023.md)。人工验收未代签，未推送。

---

# 上一轮交付：恢复3D角色，地图完整外观仍未完成（2026-11-04）

- Owner：Codex-20261104-3DRestore；本轮新增DONE **1项M6-016**。M6-015的默认2D方向改SUPERSEDED；M6-017地图外观BLOCKED、M6-018完整动作/视觉质量TODO，不能再把技术预览通过当完整资源交付。
- 默认恢复DNFArt 3D骨骼模型，修复朝向、17动作残留通道、受击骨架错误及短片段过渡。包内实际查看跑跳/普攻/受击姿势，人物不再是只播待机的2D贴片。模型仍为低精度学习原型，死亡播放/倒地/起身未完成。
- 固定开发包2026-11-04T15:05:36+08:00，源码161735e（功能实现f3a6b06）。完整回归919/919，公会/训练场包内专项均1/1，TestPackage通过，7份旧存档不变。
- Run.ps1默认自动通过GameFlow进入公会图，-Menu打开菜单；左上角显示Map: L_DNFM_Guildagit。当前已启动并从真实包日志确认自动进图。
- **地图仍缺原色贴图及原版地形，当前灰色卡片/占位地面不等于完整地图还原。** 详见[M6-017](Docs/Tasks/Reports/M6-017.md)。
- [本轮交接](Docs/Tasks/Handoffs/2026-11-04-Codex-3DRestore.md)、[M6-016完整结果](Docs/Tasks/Reports/M6-016.md)。用户人工验收未代签，未推送。

---

# 上一轮交付：地图与玩家显示修复（2026-11-04，2D方向已被替代）

- Owner：Codex-20261104-VisualFix；本轮新增 DONE **2 项：M6-015、M6-013**。任务板205/210 DONE、0 IN_PROGRESS；人工关未代签。
- 人物平面朝向与图集整帧修复：`ad0f8b4`；新场景默认入口、PlayerStart及走入触发区修复：`9f31fd9`。菜单顶部点 **Play GuildagitScene**，新地图与2D格斗家已在固定包内实际截图确认。
- 完整回归919/919；Build、Test -Render、Package、包内公会图/训练场专项、默认TestPackage均通过。固定包生成2026-11-04T11:05:40+08:00，源码9f31fd9，已核对exe哈希；7份既有用户存档未变。
- 原版贴图/地形缺失、灰色卡片景物、原版战斗动作缺失和Quinn敌人仍是已知限制；不能解释为资源全部替换完成。
- [完整交接与证据](Docs/Tasks/Handoffs/2026-11-04-Codex-VisualFix.md)；[人物报告](Docs/Tasks/Reports/M6-015.md)、[地图报告](Docs/Tasks/Reports/M6-013.md)。后续用户试玩后继续处理资源缺口；本轮没有推送。

---

# 上一轮交付：重新构建当前源码供用户试玩（2026-11-04）

- Owner：Codex-20261104-PlaytestBuild；本轮新增 DONE **0 项**。未改功能代码、任务状态或 Owner；构建包含 main `26a6253` 之外现有未提交的 M6-013/M6-015 工作。
- **固定开发包已于 2026-11-04T09:52:20+08:00 更新**：`Artifacts/Package/Windows/UEMMO.exe`。Build、Package 和默认 TestPackage 均通过，菜单已含 GuildagitScene；已执行 Run.ps1 启动试玩。下文审计的"包仍是11月3日"是本轮构建前的历史结论。
- **公会地图额外直接加载移动冒烟未通过**：地图可加载/渲染，X=-1.073cm、Y=0、跳高130.748cm，截图仍带菜单覆盖；不能算新关卡或玩家视觉验收通过。原版战斗动作/缺图等审计缺口仍在。
- 存档和旧验收记录已备份，测试后7个既有存档保留。exe SHA256=`FEDB047100237C9DE35C6B69C9E4757B0751EDDDCF5E2E9C44F9B01136FBB42B`；包内 build-workspace.json 补充未提交改动身份。
- [本轮详细交接](Docs/Tasks/Handoffs/2026-11-04-Codex-PlaytestBuild.md)；原始结果：`Artifacts/Tasks/M6-015/2026-11-04T09-46-48+0800-playtest-build/`。后续由用户体验，原负责人继续收尾 M6-013/M6-015；未代签任何人工验收。

---

# 上一轮审计：当前任务与资源替换（2026-11-04）

- Owner：Codex-20261104-Audit；本轮新增 DONE **0 项**；main / `26a6253`，功能工作区与任务所有者保持原状。
- **资源未替换完，固定包仍是 11 月 3 日的 `5fe9fcb`，未包含这轮新地图与 2D 玩家替换。** 静态入库与地图布局已有成果；缺失地形/贴图、原版战斗动作、普通敌人与图标替换仍有缺口。
- 总表登记 203/210 DONE；M6-013/M6-015 仍 IN_PROGRESS；任务板检查因同目录两项进行中返回 exit=1。历史全量回归已达 919/919，所以下文"回归在跑"是旧检查点；菜单截图不证明玩家与新关卡实机视觉通过。
- M7-004 的 DONE 仅有底层与自动化证据，报告/代码显示连续模式菜单入口、生产过图回调及哥布林波次尚未接入，需要补任务边界与交付。
- [完整审计](Docs/Audit-2026-11-04.md)；[审计历史交接](Docs/Tasks/Handoffs/2026-11-04-Codex-Audit.md)。证据：`Artifacts/Tasks/Audit-20261104/2026-11-04T093500+0800/`。本轮未重新构建/运行游戏，也未代签人工关。
- 下一步：由原负责人逐项收尾 M6-013/M6-015 的游戏内视觉与进出关卡验证，提交实现与验收记录，然后更新固定开发包。原检查点完整保留如下。

---

# 上一轮交付：DNFM 原版资源替换——地图与角色（2026-11-04，检查点 1/2）

> 用户目标：用 DNFM 资源接手包替换角色与地图，多子代理并行，交付正确完善。
> 本检查点：**3 项 DONE（M6-011/012/014）**；M6-013/015 进行中（编译已过、回归在跑）。
> 先决核查：用户核查文档《地图布局与动作反馈核查.md》经独立核对全部属实（216 根对象/5,794 Transform/
> GuildagitGroundDummy 980/972/95、树样例逐位一致、Spine 282 套仅创建界面 3 条预览动画——执行会话早期
> "282 套带全套动作"的说法是错的，以该文档为准）。

- Owner：ZCode-20261104-A；main 分支本机提交未推送。基线 `0099b56` → 当前 `26a6253`。
- **M6-011 布局还原管线**（`8ab4936`/`8c0cdfa`）：`Scripts/DNFM/build_dnfm_layout_intermediate.py` 纯标准库；产出 1,160 实例（GDD 972+BG 32+装饰 156）UE 放置中间数据；坐标约定 Unity(x,y,z)→UE(x,z,y)×100（引擎源码级推导+FBX 实测锚定）；重跑哈希一致；183 映射 ID 在 UE 目录 0 缺失。[报告](Docs/Tasks/Reports/M6-011.md)
- **M6-012 公会场景 UE 关卡**（`feb9c81`/`4a643f1`）：`L_DNFM_Guildagit` 重建 1004 实例（6 禁用件 `_DISABLED`+游戏隐藏、3 镜像 Z 负缩放、材质 1010 槽全应用）；8 个缺几何实例=原版地形（GA_ground/islands/water）→ 明确标注的占位地面；二进程验证 1005 actor/25 抽样逐项一致；3 张渲染图已人工查看（树为原版平面卡片树）。[报告](Docs/Tasks/Reports/M6-012.md)
- **M6-014 Spine 格斗家入库**（`7430b8f`/`26a6253`）：303 帧烘焙帧（与 pixi-spine 真值误差 1.1e-4）→ 3 图集+3 材质入 UE；UE 5.8.3 无 FlipBook 表达式（引擎头/dll 证实）→ UV 分支等价实现；import/verify 双进程通过。[报告](Docs/Tasks/Reports/M6-014.md)
- **进行中 M6-013**：rooms.json 加行+DA 生成+trigger 自动接线全部通过；剩全量回归（在跑）+打包冒烟。
- **进行中 M6-015**：C++ 视觉替换已写完并**编译通过**（`UEMMO.PlayerVisual2D` 默认 1=原版 2D 格斗家、0=回退 3D）；随同一轮回归验收；游戏内截图证据用 `Test.ps1 -Render`。
- 已知限制（诚实记录）：原版地形/公会建筑网格缺 CAB 不在 279 内→占位地面；978/996 材质槽缺图→中性灰；**原版无战斗动作**（仅创建界面 3 条待机）→ 2D 分支攻击无换帧表现，音效/伤害数字/特效反馈保留。
- 历史交接：[2026-11-04-ZCode-20261104-A-检查点1](Docs/Tasks/Handoffs/2026-11-04-ZCode-20261104-A-checkpoint1.md)。

---

# 上一轮交付：DNFM 模型与地图素材接手包（2026-11-04）

- Owner：Codex-20261104-DNFM-Handoff；本轮新增DONE **1项 M6-010**。基线`c3aa251`，实现`57fd19e`，main本机提交未推送。现有模型未重复导入、视觉资产未改。
- 用户将自行安排其他Agent执行后续任务，入口是 [06-Agent资源接手入口](Docs/DNF-Research/06-Agent资源接手入口.md)。内容浏览器 `/Game/UEMMO/Imported/DNFM130_5_8` 已有全部279模型（含183地图/背景件）、137来源材质+2辅助、39纹理和展示图。
- 本轮补入项目本地297份可获得的原始依赖（45.9MB）与2份导航原件；`SourceAssets/Unpacked/DNFM130_5_8/handoff` 包含逐模型JSON/CSV、地图子集和缺失依赖清单，后续Agent可按UE object_path直接加载。
- 验证：新UE进程重载279/137/39通过；所有副本SHA256+长度一致；1,385不同项目文件路径与10个文档链接无失效。证据 `Artifacts/Tasks/M6-010/2026-11-04T012301_0800`；[报告](Docs/Tasks/Reports/M6-010.md)。
- 限制：154模型仍缺原底色；依赖闭包有56缺失CAB，已明确列出。导航不是UE NavMesh，原地图布局未组装，特殊Shader仍为近似。本轮不代替M6-009历史渲染验证、不重打包、不代签人工关。
- 下一步：用户安排Agent按接手文档选资产实施；无需再整批导入。资产/原始副本是本机Git忽略数据，跨机器需随项目另行复制。历史交接 [2026-11-04-Codex-DNFM-Handoff](Docs/Tasks/Handoffs/2026-11-04-Codex-DNFM-Handoff.md)。

# 上上轮交付：DNFM 静态资源适配（2026-11-04）

- Owner：Codex-20261104-DNFM-UE；本轮新增 DONE **1 项 M6-009**。基线 `51c9d38`，实现 `4d96388`，main 本机提交、未推送。当前任务表 204 项、199 DONE、0 IN_PROGRESS；M5-H03 REVIEW、M7-005 BLOCKED 保持用户人工关状态。
- 已导入 `/Game/UEMMO/Imported/DNFM130_5_8`：279 Static Mesh、137 来源材质 + 2 辅助材质、39 Texture、独立 `Maps/L_DNFM_ResourceGallery`。资产和源素材在本机忽略目录，脚本/文档已提交。
- 验证：279 FBX 新 Blender 进程回读（顶点/面/UV/实际RGBA）；UE 导入→新进程重载全部279；39纹理寻址（23Clamp/16Repeat）；渲染展示与3个近景共4张新PNG已查看；Test -Render success=true（X421.415/Y-463.878/跳高130.743cm）。无C++变更，未重打开发包。独立代码复查问题已修复。
- **内容限制**：154 网格有缺失底色引用，公会36材质依赖首包缺失CAB；中性占位明确标记。自定义Shader为参数可追溯的UE近似，不是原效果完整复现；原动画/Spine/粒子未转换为UE运行逻辑。静态资源适配完成，不冒充完整原版地图或模型外观已还原。
- [任务报告](Docs/Tasks/Reports/M6-009.md)、[使用与重跑说明](Docs/DNF-Research/05-DNFM静态资源UE适配.md)。证据：`Artifacts/Tasks/M6-009/20261104T005828+0800`；预览 `Artifacts/Tasks/M6-009/DNFM_UE_Gallery.jpg`。历史本轮记录见 [handoff](Docs/Tasks/Handoffs/2026-11-04-Codex-DNFM-UE.md)。
- 下一步：可直接打开展示图检查或拖网格入自己的地图；若继续恢复原色，需要提供缺失纹理包。既有人工试玩仍由用户签收。本任务无需新授权即可使用已落盘素材。
