# UE-MMO 多职业、表驱动技能与等级、双房间地城扩展设计

状态：已记录独立角色、六技能包含 Z 上挑及仅本机私人学习试玩的决定；格斗家造型、两房间编排和跨角色共享待审阅。修订日期：2026-11-01（Asia/Shanghai）。本文仅定义未来工作，不代表功能已实施或验证。游戏源码、任务板与固定开发包在本轮均不修改。

## 1. 目标、边界与现状

目标是保持现有 UE 5.8.3 单机 3D 横版原型全部已验收功能，追加可配置的多职业、各职业技能、职业等级曲线和一个连续双房间地城。X 是横向，Y 是可走的地面纵深，Z 是高度；绝不锁 Y。当前基线的枪、载具、背包、自由选关、武器状态、原有四个攻击、存档和奖励必须继续可用。多人、账号、服务端不进入本期。

已核对的实际入口：

- Data/combat-attacks.json 与 Source/UEMMO/Combat/AttackDefinition.*、AttackCatalog.*、CombatComponent.*：四个既有普攻/上挑/空中攻击定义，60 Hz 窗口、150 ms 输入缓存和统一命中入口。CombatComponent 的 Free 状态与取消链仍把 Light/Launcher 映射到固定攻击 ID，不能仅加 JSON 行就声称职业技能会执行。
- Source/UEMMO/PrototypeCharacter.cpp：方向键移动，X 普攻，Z 上挑，C/Space 跳跃，F2 复位；QWERASDF 是八个槽位意图，其中 Q 还绑定既有武器开火，普通技能槽当前仅记数，没有技能效果。
- Source/UEMMO/Profile/ExperienceCurve.* 与 ProfileSubsystem.*：现有等级 1..10、升级所需 XP=100×当前等级；基础 HP/攻击/防御是代码公式。RewardService 的单房间经验 50 是代码常量。
- Source/UEMMO/Persistence/ProfileSaveGame.h 与 ProfileSaveService.*：当前存档 schema=3，单一 Profile_ A/B/Index，带物品、装备、弹药与房间通关记录；启动加载与损坏回退已存在。不能通过改类 ID 或文件名直接让老存档失效。
- Data/rooms.json、Source/UEMMO/Room/* 和 Profile/GameFlowSubsystem.*：已有 room_combat_01、room_combat_02 两张独立战斗图、刷怪规则、清怪开门、自由选关及逐房间结果/奖励。两图目前不是一局连续双房间地城。
- Data/CombatSystem/manifest.json 和 CombatDataTableParser.cpp：战斗表白名单严格拒绝未知表及未登记表，新增职业文件不能简单扔进该目录。
- D:/DOF/DNF-REA-Output：有 PVF 配置、NPK/IMG 索引、局部 PNG、音频及映射报告；来源是研究证据，不把 DNF.exe、原 Squirrel 脚本或未授权素材直接放进 UE 项目。

## 2. 设计选择

推荐 A：扩展现有 Profile、Combat、Room/GameFlow 域，各域仍只有一个权威所有者；职业、技能、等级和地城序列另建严格校验的配置目录与生成资产。优点是复用已完成的存档、攻击时钟、统一伤害、房间规则和打包管线，回归面可分段控制。缺点是存档 schema、角色启动与技能调度需要认真改造。

B：为多职业平行建立第二套角色/战斗/存档框架。虽然初看改动少，但会产生两个命中、奖励和存档真相源，易破坏既有功能；不采用。

C：按 PVF/Squirrel 逐字段、逐脚本移植整套原游戏。数据规模和版权边界均远超本期，而且现有 DNF.exe 核心行为未反编译；不采用。

用户已决定多个独立角色，每人一个固定职业、独立等级和存档；只在主菜单切换角色，不在战斗中转职。库存与通关是否跨角色共享仍待确认，当前方案默认各自独立。旧存档迁入技术职业 prototype，保持当前行为。首个新职业候选是徒手 fighter；其余职业依同一表结构追加。用户还决定首职业六技能包含现有 Z 上挑，因此默认规划为 Z 上挑加 W/E/R/A/S 五个热栏技能，D 槽保持原意图但暂不装新技能。fighter 和五个具体技能仍是待确认候选。

## 3. 配置源与数据模型

新增 Data/Professions/manifest.json、professions.json、level_curves.json、skills.json、skill_levels.json、loadouts.json，以及 Data/dungeon_sequences.json。全部采用 schema_version、table、rows 的当前风格；解析器明示支持的表，manifest 注册必需文件。不要在当前 Data/CombatSystem 目录加入未知 JSON，除非先扩展其严格解析器与生成器。生成资产放 Content/UEMMO/Professions、Content/UEMMO/Skills；制作工具在 Scripts/Editor 里用 UE Python API 创建并保存，二次进程重载核对；纯文本源表为权威，uasset 不手造。

表与字段契约：

| 表 | 一行主键与主要字段 | 约束 |
|---|---|---|
| professions | profession_id、display_name、curve_id、mesh_path、skeleton_path、locomotion_set_id、starting_skill_ranks、allowed_weapon_modes、presentation_id | ID 全局唯一；不得引用不存在的曲线/资源；prototype 为旧档兼容职业 |
| level_curves | curve_id、level、xp_to_next、base_max_hp、base_attack、base_defense、base_max_mp、base_mp_regen、skill_points | 对每条曲线从 1 连续到 max_level；全为有限且合法数值；末级 xp_to_next=0；旧 prototype 1..10 与旧公式逐项一致 |
| skills | skill_id、profession_id、max_rank、slot_eligibility、effect_policy、attack_id、effect_parameters、icon_path、montage_path、vfx_path、sound_path | 跨表引用闭合；每个效果策略有专门校验器；视觉路径空值须明确 placeholder 标志 |
| skill_levels | skill_id、rank、required_character_level、sp_cost、cooldown_s、mp_cost、damage_coefficient、effect_parameters | rank 从 1 连续到 max_rank；升级条件和数值按职业校验；每个 rank 用当前等级查表，禁止猜增量公式 |
| loadouts | profession_id、slot_index、skill_id、equipment_id | 1..8 槽唯一；只能装备该职业已学且 rank≥1 的技能；旧武器/物品 ID 保持不变 |
| dungeon_sequences | dungeon_id、display_name、ordered_room_ids、completion_reward_table_id、xp_reward、entry_requirement | 第一版只允许两个不重复、已存在的 room_id；中途房不结算奖励；终房只结算一次 |

时间单位与空间单位跟既有工程统一：秒、厘米、60 Hz 逻辑帧。源 PVF 的原始值保存在研究对照表，不直接喂 UE 的 cooldown_s、MP 或伤害值。项目内容表选一套可调数字并记录出处；测试验证每个字段而非假定原游戏单位。表内不允许任意函数名、动态脚本路径或可执行表达式；effect_policy 只能从 C++ 已实现的策略白名单选择。

运行时建立只读的 FProfessionCatalog/FSkillCatalog/FProgressionCatalog，解析、局部与跨表校验全部成功才一次性发布快照；缺职业、技能、攻击、音效关键资产时返回明确错误。旧逻辑使用 prototype 的等值配置行，保证旧测试输入输出不变。新表规范化后的内容哈希作为 ProfessionConfigRevision，写入存档和证据；仅增新行且旧 ID 含义不变时允许旧档加载，破坏性改名需要显式迁移。

## 4. 职业与等级运行时

UProfileSubsystem 仍是当前激活角色进度、装备、库存和奖励的唯一所有者；只增加 ProfessionId、已学技能 ID 与快捷栏映射的只读快照/受控修改 API。FProfileSnapshot 新增 ProfessionId、MaxMP 与技能状态；World 内 HP/MP 是瞬态，不把战斗中剩余值固化为基础属性。FStatCalculator 保持装备最终重算入口，用职业等级表给基础行，再叠加原有装备 bonus。旧 FExperienceCurve 静态接口保留并返回 prototype 的原数值，防止现有调用和测试突然改变；新职业调用带 ProfessionId 的曲线接口。

每职业的最高级由其 level_curves 最后一行决定，首批 prototype 的 1..10 原样固定，fighter 可先配 1..10；未来扩容通过增加连续等级行和对应存档边界测试完成。AddXP 的正数、溢出、多级连升和满级语义沿用原 Profile 的现行约定；表驱动需求值替换公式，但旧职业必须得到相同结果。等级上限、奖励 XP、技能解锁均可通过表调整；调整时先验证旧角色是否仍合法，不会静默降级或洗掉技能。技能可解锁不等于自动装备，快捷栏保持玩家选择；移除已解锁技能的配置必须有迁移规则。

技能 rank 与角色 Level 分开：Profile 保存 SkillId→Rank 的稀疏表，NewProfile 只按 starting_skill_ranks 初始化；按职业 level_curves 给的 skill_points 累积获取 SP，学习/升阶要检查前置、角色等级、可用 SP 与上限，成功一次扣一次 SP 并保存。免费自动解锁须在技能表显式标出，不依赖“角色升级就自动送所有技能”的暗规则。需要同时保存 UnspentSP，旧 prototype 迁移时学会既有四招的固定 rank 1 且初始 SP=0，避免把旧玩家直接锁出普攻；它们不从技能栏消费 MP。等级变动、技能升阶后的属性和费用从对应等级行重新求值；重复购买、失败事务不扣 SP。skill_points 必须按每级“本级获得”定义，迁移旧档不为过去级别倒补 SP，除非产品明确提出补偿计划。

职业选择只在无进行中战斗、无正在保存的主菜单执行。选择后加载对应角色到唯一 UProfileSubsystem，再进入原 GameFlow。进入房间前从所选职业配置解析网格、动画与技能目录，并在未加载完整时拒绝开局。装备、奖励与房间通关记录归角色 ID，不能跨角色共享。旧武器系统仍按现有装备实例 ID 运行，新职业只决定合法动作/武器模式，不复制另一个武器库存。

## 5. 多角色存档与旧档迁移

新增角色目录索引 Roster 的 A/B/Index 事务，记录 CharacterId、ProfessionId、显示名、存档前缀和活动标记；各角色沿用现有 FProfileSaveService 的 A/B/Index 强校验与故障回退。新角色前缀从 CharacterId 稳定派生，现有 Profile_A、Profile_B、Profile_Index 仍是旧角色原始槽。新建角色时先为其分配 GUID、写入可恢复的 pending roster 项，再完整保存角色并读回验证，最后提交 active roster 项；中途失败保留可诊断 pending 项，不覆盖旧档。

UProfileSaveGame 由 schema 3 升到下一版，加入 ProfessionId、SkillId→Rank、UnspentSP、技能槽映射、ProfessionConfigRevision；在 FProfileSerializer 中显式迁移 v1→v2→v3→新版。v1/v2/v3 旧档的 ProfessionId 填 prototype，基础四招固定 rank 1，其等级、XP、物品 InstanceId、装备、弹药、已领奖 SettlementId 和 RoomClears 原值不变。迁移只在加载内存时发生；仅在新槽写入、读回、哈希校验成功之后更新索引。未知更高 schema、缺职业、损坏索引/双方存档都应拒绝载入并保留原文件；绝不以新建角色伪装修复。保存时角色 ID、职业 ID、配置版本与当前 UI 选择要一致。

具体启动顺序必须同步修改 GameFlowSubsystem：先探测 Roster 索引；有合法 Roster 时只按选中角色的专属槽加载，不能仍在 Initialize 阶段抢先加载固定 Profile_。无 Roster 且存在旧 Profile_ 时执行一次迁移：把旧 A/B/Index 保持原字节只读，写并读回以原 CharacterId 派生的新角色槽，再提交 Roster；失败时保持旧档及无新 Roster 状态、下次可重试。无 Roster 且完全无旧档时按现有体验创建 prototype 新角色并进入主菜单，菜单另提供“创建 fighter”；这是兼容默认行为，不暗中选新职业。若 Roster 存在但损坏，报恢复错误并保留文件，不能当成“未初始化”再次导入旧档造成双角色。Roster 必须以独立事务记录当前角色 GUID 与各角色前缀，切换角色前先保存当前角色并读回，失败则拒绝切换。

如果产品改为单角色可切换职业，替代方案必须把进度、技能栏和装备绑定按 ProfessionId 分页保存在同一 CharacterId 下，同时决定共享库存、角色等级和房间记录的规则。这不是在上述独立角色方案上改一个字段即可完成的实现细节。

## 6. 技能输入、执行、战斗和表现

一条技能链是“既有输入动作 → FSkillIntent(slot, active character) → USkillLoadoutComponent → UCombatComponent/统一伤害入口 → 事件表现”。输入层不直接扣血、生成掉落或写存档。保留方向键移动、X 普攻、Z 上挑、C/Space 跳跃、F2 复位、I 背包与现有枪/车输入。Q 当前负责开火，不能直接改成技能首键。首职业使用 W/E/R/A/S 五个已有槽位，Z 上挑计入六技能；D 保持原意图但暂不装新技能。Q 继续原开火和原槽位计数，F 保留第八槽占位。所有既有槽位计数与操作日志继续记录，不以是否触发技能取代它们。后续如需让 Q 也放技能，另设经过验证的职业/武器输入上下文与 UI 提示，明确同一帧只产生一种效果。

USkillLoadoutComponent（玩家 Actor 上）只持有已选职业的 8 槽映射、当前 MP 与技能冷却截止时刻；是否学会、等级上限和装备都向 Profile/Catalog 查询，不造第二份永久状态。TryActivateSkill(slot, clock) 返回 Accepted 或明确拒绝原因：Locked、WrongProfession、InsufficientMP、Cooldown、Busy、MissingAttack、MissingAsset、InvalidContext。触发时先做无副作用预检；接着请求现有 UCombatComponent 开始指定 AttackId；只有 UCombatComponent 成功接受后才扣 MP 和写冷却，事件输出携带角色 ID、技能 ID、攻击实例 ID 与配置 revision。被受击打断的技能默认不退款；死亡、切图、装备变化时取消的是动画与旧动作实例，不能重复提交伤害。

现有 UCombatComponent 内 Light/Launcher 的固定映射与四招取消链保持为 prototype 默认。为新职业加只读 FProfessionAttackSet，声明普通攻击起手、上挑、空中攻击和合法取消目标；新技能从槽位进入同一个 UCombatComponent 时钟、命中窗与统一命中去重。扩展 Data/combat-attacks.json 与 UE 资产生成器时必须把固定四招清单变成“旧四招必须存在 + 经验证的新增招式”，旧四招名称和参数不得改变；Data/CombatSystem 中现有 melee_weapon 四招全集校验也保留，职业技能作为独立合法动作集合验证，不能绕过或默改旧武器约束。

技能策略白名单首期仅实现本职业用到的 SingleMeleeHit、GroundArea、Projectile、SelfBuff；MultiHit、TargetDebuff、Grab 留作后续扩展的枚举保留位，不宣称已实现。每种已实现策略明确自身的 attack_id、命中组、受击类型、范围和目标筛选。首个职业用 SingleMeleeHit、GroundArea、Projectile、SelfBuff 覆盖六招；若某技能的动画/判定尚未具备，不填虚构打击表现，而把该行标为不可启用并阻止该职业进入可验收版本。远程技能生成的弹体必须复用既有 Projectiles 路径，伤害通过 ApplyUnifiedHit 与统一 Ledger 提交，不调用第二套 ApplyDamage。技能状态对同一目标同一 HitGroup 只能命中一次；多段命中须有不同受控 HitGroupId。X/Y/Z 命中体积、纵深容差和脚底原点沿用当前约定，Y 不做零值退化。

冷却使用已定义的可暂停游戏时钟，冻结规则与现有输入时钟一致；配置是秒，禁止把 PVF 毫秒原值直接当秒。冷却与剩余 MP 属一局的瞬态状态，连续两房间过图时由 DungeonRunState 保存“剩余秒数”和 MP；新地图加载后恢复到本地图时钟，绝不直接跨 World 比较旧绝对时间戳。普通自由选关仍走现有初始化/清理语义。大厅存档默认不保存战斗中冷却，防异常退出后留下失效截止值。等级提升增加 MaxMP 时不自动回满；技能树和快捷栏在菜单与房间内显示已解锁状态、MP、冷却、拒绝原因，打开背包等 UI 时不得让键位漏入战斗。

表现层只消费成功的技能开始、命中、被打断和完成事件：职业网格与骨架、蒙太奇、Niagara/材质特效、SoundWave、图标均通过软引用。关键动画未提供时仅在明确标记占位的开发预览中用引擎现有素材；正式验收要求角色姿态与真实碰撞窗对应。新增技能动画采用可回归的源动画长度和逻辑帧窗口映射；保存 uasset 后在新 UE 进程重载并拍摄动作生效帧/受击画面验证。游戏运行时不读取 DNF NPK/PVF，也不在 UE 内执行原 Squirrel 脚本。

## 7. 连续两房间地城

Data/rooms.json 的 room_combat_01、room_combat_02 和自由选关入口保持原样。Data/dungeon_sequences.json 新增一个“连续模式”ID，按顺序引用现有两张房间图，独立于现有菜单自由选关。这个序列不是简单地在菜单中列两关：用户进入地城后，第 1 房清怪 → 开门 → 到门口 → 过图进第 2 房 → 清怪 → 完成整个地城 → 一次结算/领奖 → 返回菜单。死亡或主动退出记失败并回菜单；不会把第 1 房的临时进度当作通关，也不重复掉落。

在 UGameFlowSubsystem 增加 GameInstance 生命周期的 FDungeonRunState：DungeonId、选定 CharacterId、当前房索引、RunGeneration/SettlementToken、带状态的 HP/MP、按 SkillId 保存的冷却剩余值、源配置 revision；只保存本次运行态，不写入持久档。进入时要求无其他正在进行的 run。ARoomExit 在 BeginExit 成功时发布带 RoomId/RunId 的 OnExitAccepted；地城协调器仅接受当前序列中刚清空的房间和当前代数，快照瞬态状态，异步打开下一张已有地图；新 World 的 ARoomTrigger 仍从该房的 RoomDefinitionAsset 开波。加载失败恢复到安全菜单、原存档不动、错误可见。过图时旧 World 的倒计时、弹体、敌人与委托失效，新 World 分配新 Epoch，旧命中不能窜入第二房。

第 1 房运行结果是地城内部进展：不弹当前逐房间胜利结算 UI，不创建 PendingReward，不记录一次完整地城奖励。第 2 房成功才通过现有 RewardService 创建一次 SettlementId 对应的 PendingReward，玩家认领时沿原先 A/B 事务发放 XP、物品与原 RoomClears 最终房记录；新增 DungeonClears 作为独立值型计数。这需要在下一版存档里同时序列化、校验并迁移 DungeonClears，且纳入 A/B 完整性摘要；不能只在内存里计数。失败、退出、重复消息均不得结算或重复计数。普通从菜单单独进入 room_combat_01/02 时，现有逐房间结算行为不变。RewardXPPerClear=50 的旧接口与旧测试保留；连续模式的 xp_reward 从地城表取，经同一个 RewardService draft，且必须记录原房间与地城身份以便审计。

原洛兰/哥布林资料只用作关卡节奏和近战敌人参考：pvf/dungeon/act1/lorien.dgn.json 给出 2×3 网格、入口与 BOSS 坐标；pvf/monster/goblin/goblin.mob.json 给出近战类别、动作/攻击引用。不能把原游戏地图房间编号直接等同于本工程两个 room_id，也不把 2D 地图瓦片当成 3D 几何。首期敌人行为可基于当前 melee_grunt 通过现有敌人表配置调整，模型和动作留到资源包到齐后接入；现有 melee_grunt 仍保留供旧关卡。

当前 Data/enemies.json 的 melee_grunt.attack_power=0，原敌人并不构成真正的扣血压力。新切片另加配置 ID，例如 dungeon_goblin_proto，给可测的正攻击力、预警/追击/纵深容差，引用其专属模型与动作；连续地城两房的 waves 指向它。旧 melee_grunt 的 ID/参数不改。敌人的受击、死亡和计数继续走现有 HealthComponent、UnifiedHit 与 RoomSessionSubsystem。

## 8. UI、日志、错误和兼容

主菜单先显示角色列表，再显示当前角色可玩的地城/自由选关；新建角色要明确选职业，旧档在迁移后显示 prototype。角色卡显示等级、XP、职业、可用技能数；战斗 HUD 显示五个热栏槽位图标、MP 与冷却，并保留 Z 上挑提示；结果页明确标“房间结算”或“完整地城结算”。显示层不自行计算等级或技能解锁，只读 Catalog/Profile/SkillLoadout 的快照。

当前 PrototypeHUD::HandleRunEnded 在每一房结束时调用 PrepareRoomRewardDraft、显示结算 UI，并通知 GameFlow 进入 Result。连续模式必须在该处理函数调用任何三者之前，通过 DungeonRunCoordinator 的纯只读分类器检查“当前序列、当前角色、当前房索引和结果”。中间房清空只发 DoorUnlocked/IntermediateClear，不进入 Result、不生成奖励、不抢 UI 输入；终房或失败才走现有结果流程（但中途失败不发胜利奖励）。ARoomExit 获准 BeginExit 后由协调器发起下张地图的 OpenLevel；原自由选关没有 active DungeonRunState，完整沿用旧 HandleRunEnded 路径。这样也能保证先结束会话、后走门的事件顺序不造成重复领奖。

日志输出以现有 OperationLogSubsystem 记录角色选择、技能意图/拒绝原因/开始/取消/命中、房间进出、奖励草稿/认领、配置 revision。所有拒绝记录可诊断字段名与 ID；配置不合法时拒绝进入新职业或地城，不自动回退 prototype 伪装成功。旧角色及旧自由选关对缺少新职业表的开发分支也须有明确可测试的兼容门，不能在打包后静默跳过新内容。

明确保留的基线：旧存档原文件、等级 1..10 的旧数值、Q 开火、X/Z/C/Space、F2、F1、I、枪弹/弹匣/载具、老两房间自由选关、已有 60 Hz 命中与奖励幂等。新职业最多加新路由与配置，不平行重建敌人、武器、血量或伤害服务。

## 9. 美术资源：用户提供与工具交付

用户希望以已提取的 DNF 2D 图片为参考，用 Blender 制作 3D 角色和素材。这一新指示明确改变了先前“禁止生成角色美术”的项目偏好；实施前应将此特定例外写入 UE-MMO 的 AGENTS.md 与新任务卡，原有“不使用盗版提取资源、不自行采购”的限制继续生效。能读取或提取 DNF 文件，仍不能据此认定拥有改编、分发许可。只有单视角 2D 帧时不存在唯一的背面、厚度、骨骼或 3D 动作轨迹，不承诺一键转换成准确模型。

首职业功能开工之前，先完成一个可导入、可追溯的素材包。用户已确认当前仅在本机私人学习试玩；提取图像与 Blender 研究试样先放 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/model-reference，与可公开交付素材隔离，不推送远端或发布。若将来改变为公开发行，必须重新核对 DNF 图像的改编/使用授权或改用授权的替代参考。文件可读取不等于已获授权，玩法逻辑研究与具体美术表达分开管理。

| 由你提供/确认 | 最低内容与技术规格 | 我在收到后可完成 |
|---|---|---|
| 使用权与来源 | 每包作者、原始来源链接、许可证原文或授权书、允许用途/再发布范围、版本和 SHA-256 | 许可清单、来源隔离、不可发布标记；不明权利资源不进可交付包 |
| 职业 3D 外观 | 选择一组 fighter 外观版本、侧面/动作关键帧；提供期望的正面、背面和比例参考，或接受我先提出推测方案 | Blender 原型网格、骨骼、材质、转台预览和可导入 FBX/GLB；经审阅后在 UE 做 SkeletalMesh/碰撞 |
| 职业动作 | 确认六招动作风格、视觉优先级与可接受的原画还原程度；可选提供更多参考视频/帧 | 从有权参考帧分析动作相位，以合规骨架及 Blender 动画制作/重定向待机、X/Y 走跑、普攻、上挑、五热栏技能、受击和死亡；逐招核对命中帧 |
| 六技能特效 | 确认每招颜色、落点、范围、阶段和时长；可选提供有权使用的效果贴图 | 从参考与程序化材质生成技术原型，在 UE 以 Niagara、材质和 Decal 建立表现并截图审阅 |
| 图标和 UI | 确认是否拥有原技能图标的目标用途许可；没有则选独立风格 | 经授权图标规范化，或绘制不直接复制原图的临时技能符号；制作锁定/冷却态 UI |
| 音效 | 确认原音效是否有使用权及期望音色；可选给授权音频 | 用获准音频或现有 Kenney 音效先做占位，产出 SoundWave、并发与事件绑定；无许可证不直接复用原声音 |
| 一种敌人 | 选择 goblin 外观参考或明确接受原创化敌人；确认缺失视角允许设计补全 | 同一 Blender 流程做敌人原型、骨骼与动作，接既有 melee_grunt 型行为 |
| 两房间场景 | 确认原关卡仅作逻辑参考还是可用原贴图；确定期望美术方向 | 复用已有两房间灰盒并建独立 3D 场景件，保留 Y 纵深/导航/遮挡与门态反馈 |

我会给选定的提取帧及新资源建立 CSV/JSON 清单：id、kind、path、source、license、owner、skeleton、scale、forward_axis、root_motion、notes。约定 UE 厘米、X 横向、Y 地面纵深、Z 向上。敌人和两个房间素材可以跨职业共用；各职业仍需单独的网格、动作、六技能效果与图标清单。你不必事先制作现成 3D 模型；你需要决定使用范围、外观版本和推测视角的审阅结果。

工具可在 UE 编辑器 API 内导入有权源文件、生成并保存/复载 uasset、制作材质实例和简易程序化特效、Retarget 动画、搭关卡与截图验证；也能规范化已提供图标和纹理。现有 Manny、Quinn、Kenney、Quaternius 可以作为待核对的占位候选。Blender 路线分六步：①从 fighter 的 avatar skin/hair/coat/pants 等分层 NPK 与 goblin 包中选定具体角色版本，按配置/IMG 路径导出关键帧 PNG；②清理透明边、分离服饰，做侧面图集与必要的正面/背面设计假设；③在 Blender 中摆参考平面、统一体型与尺寸，以授权人形基础网格或程序化低模为起点构造三维头身/服饰，制作 UV/材质；④绑定骨骼、权重和表情/衣物遮挡，先作转台与行走/转向试样，再制作/重定向六招及受击动作；⑤渲染固定侧视及斜视转台，逐项和 2D 参考对照，标注推测部位由用户审阅；⑥导出 FBX/GLB 后经 UE 编辑器导入、动画蒙太奇、重载和包内验证。单视角角色转网格的简单挤出/贴片只能作 2.5D 原型，不能通过完整 3D 角色验收。Blender 脚本可自动完成参考板、命名、材质节点、导出和部分低模几何；有机人物拓扑、权重与六招动作仍需多轮检查。Blender 5.2.4 LTS 候选版已按用户提供路径验证 --version 与后台 Python 启动；后续每阶段仍须重新打开 .blend 并验证导出文件。

资源按 A/B/C 三级验收：A=来源、允许用途和项目规则核验；B=Blender 产物可导入、骨架与六招动作完整且用户看过多视角试样；C=UE 新进程重载后的侧视渲染、命中帧和声音实测。用户要求资源齐了再动手，因此先完成资源研究/建模阶段，A/B 通过后才领取首职业功能代码任务；C 是职业功能标 DONE 的必要条件。明确批准免费素材作临时 A/B 占位时须标记临时，不得称成品美术。若最终用途需要公开发布，权利核验属于 A 的必要条件。

## 10. 第一个职业建议包

建议首职业是徒手 fighter，以其技能结构作设计参照。用户确认六技能包含 Z 上挑，因此推荐候选组合为 liftupper（Z 上挑）+ singlekick（W 单踢）+ damagelowkick（E 下段踢）+ throwsand（R 抛沙）+ earthbreak（A 地裂）+ stomp（S 踏地）。原始最低等级分别为 1、1、5、10、10、10，仅用于理解源游戏技能层次；本工程解锁、冷却、MP 与伤害须由新表定义并通过测试。Z 上挑直接使用既有 launcher 动作 ID，不重复建另一套上挑，D 槽保留空位和原意图计数。六招图标引用 Character/Fighter/Effect/SkillIcon.img，但须通过授权门，不能因为资源已提取就直接并入可发布工程。抛沙使用已有投射物机制，地裂/踏地需要范围攻击策略，各招的命中组与 Y 纵深范围在资源参考和配置中标明。

最小可批准包：一个已绑定且穿完整服装的职业网格与骨架、基础运动/受击等约十类动作、六招专用动作、六组效果定义或已批准的程序化暂用方案、六个图标、六组音效或已批准的 Kenney 占位、一只带胸甲/下装/武器的敌人模型与动作、两房间共用灰盒或场景包、每项来源/使用范围。具体服装部件、怪物护具、转台与穿模验收见 09-首职业与怪物服装细化设计-待审阅.md。即便用户选 Blender 制作，也须先完成这些导出与验收；某招可能需要多个动画和音效，所以此处是最低类别清单，不是文件数上限。

## 11. 实施顺序与验收

待用户确认本方案与资源门禁后，所有新功能先在 UE-MMO 仓库 TASKS.md 追加按依赖排序的任务卡；一项一项领取，每项完成当场写报告、任务状态和提交 SHA；每完成三项或中止写 HANDOFF。开工和收工运行 CheckTaskBoard。现有 191/195 DONE 和待用户试玩的 M5-H03 不自行改写；新需求用新 ID，不冒用旧 DONE。超过 90 分钟的实现工作按规则拆成更小卡。

1. **资源门禁与旧行为快照**：角色存档与六技能计数已确认。先确认 Blender 可执行文件、研究/发布用途及可用参考的许可；将用户本次 Blender 建模要求写为 AGENTS.md 的项目特定例外。按 IMG 索引选 fighter 造型与哥布林候选，先做侧视及转台可行性试样并交用户审阅，再逐步制作 Blender 角色、敌人及六技能资源；每阶段记 SHA-256、模型/骨架/动作覆盖和缺口。只有 A/B 验收通过才开始功能代码。另保存当前固定包 identity、旧 Profile_ A/B/Index 的真实存档副本与自由选关/枪/载具基线。不能删除旧档制造首次运行成功。
2. **职业与等级配置底座**：新增严格解析/校验与 prototype 等价行、fighter 新行；曲线要求逐级连续，验证重复 ID、缺引用、越级、坏数值、满级、旧数值不变；生成、保存、二次进程复载资产。此阶段不触碰原存档文件。
3. **多角色与迁移**：实现 Roster 事务、独立角色槽、新 schema、v1/v2/v3 升级、角色选择 UI。测试旧档的角色 ID/等级/物品/弹药/已领奖和清关记录逐值不变、坏档回退、未来 schema 拒绝、两个角色等级/背包/装备/清关隔离。真保存后重启验证。
4. **首职业六技能**：配置数据、现有 Z 上挑与 W/E/R/A/S（按确认结果可加 D）输入、MP/冷却、攻击窗口、统一命中及表现。最终确认的六技能分别测未解锁、升级解锁、命中、纵深打空、冷却、打断、死亡、复位、过图；资源导入和技能帧截图同步验证。
5. **同一趟双房间**：新地城序列表引用现有两张图；首房清空仅开门过图，终房清空一次结算。测第一房失败、第二房失败、切图失败、重复领奖、过图 MP/冷却、旧世界弹体/委托清理；旧自由选关的逐房间结算继续通过。
6. **第二职业证明表驱动**：选一名能力结构不同的职业，提供其资源包，通过增加配置而非复制 CombatComponent 接入。先以一远一近或一攻一状态两项能力验证架构复用，再补齐该职业六技能及对应资源，完整验证其等级曲线、装备许可与旧职业不同；此阶段单独过资源 A/B/C 门。两项能力通过只算技术中间检查，不算第二职业可玩交付。

每阶段留任务专属日志、JSON、截图与测试报告。源码修改要完整 Build；输入/运动要 Test；模型/地图/材质/特效要 Test -Render 并实看图片；资产生成需新进程重载；打包固定到 Artifacts/Package/Windows，包内执行多关卡场景与全量回归。命令退出码为 0 之外还要本次 JSON 报告 success=true。最终需用真实键盘完成职业选择、六招、Y 纵深打空、连续两房、存档重启和失败重试；现存人工 H 项只能由你反馈签收。

## 12. 需要你拍板的五项

已确认：多个独立角色；六技能包含 Z 上挑。第 10 节的 fighter 与五个热栏技能是推荐候选，尚未由用户逐项确认。用户希望优先用提取出的 DNF 2D 图像作为 Blender 建模输入。

1. 当前用途已确认：仅本机私人学习试玩；研究试样不公开，也不自动转为可发行素材。用户的新建模指示在实施前应同步写进 AGENTS.md 的项目特定例外。
2. Blender 已由用户提供并于 2026-11-01 实测可用：D:/DOF/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender.exe，版本为 5.2.4 LTS Release Candidate。缺少的前/背视角由建模 Agent 先作明确标注的推测方案，并在转台审阅时交用户确认。
3. 两房间是否复用 room_combat_01→room_combat_02 的连续模式（推荐），还是新建两张关卡图？
4. 跨角色共享：本文默认库存、装备、通关与货币均不共享，仅全局音量等设置可共享；是否同意？

这些决定会影响用户可见交付，需在执行前确认。其余细节按本方案和未来独立任务卡推进。资源未通过 A/B 门禁前，不开始首职业功能代码。
