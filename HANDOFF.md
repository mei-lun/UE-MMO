# 最近一轮交接

## 本轮：批次78 完成交接（3/3：M5-035 DONE）

- Owner：GLM-5.3-Flash-20261009-B（接续用户指令「继续执行 M5-035 的剩余步骤」）。完成时间：2026-10-09T12:05+08:00；分支 main。任务 M5-035 标 DONE（报告 [Docs/Tasks/Reports/M5-035.md](Docs/Tasks/Reports/M5-035.md)）。
- 本批第 3 项完成。批次 78 成果：**M5-034**（dcb8095 / b76c057）+ **M5-035**（实现 567ed059031b57bd9ed4b439fdbb1dee8e9dc41b，报告/总表待本笔文档提交）。
- M5-035 关键成果：
  1. 029..032 策略生产分派接线（`MakeWeaponProjectileMotionPolicy` 唯一 choke point）+ burst 独立分支（`BurstCount > 1` 挂 `FBurstFirePolicy`，授权面扩展 BurstFirePolicy.*）。
  2. **弹丸 spawn 泄漏根因修复**：`FWeaponProjectileSpawner::Spawned` 跨 commit 累积陈旧弱引用，`PelletTotal = Min(Pellets.Num(), Spawned.Num())` 只扫 index 0 死指针静默 continue → CommitSpawn 前 `Spawned.Reset()`（cast 到具体类；这是 death 12 发只落 1 伤、burst 3 发只落 1 伤的公共根因）。
  3. retire 即 `Destroy()`（LifeSpan 残体阻塞后续弹道）；测试修正：爆炸 overlap 用 ECC_Pawn 对象类型（BlockAll=WorldStatic 查无假人）、reserve 期望 57、post-reload 24 帧。
  4. 13 处 M5_035_DIAG 插桩全移除（Source 树零残留，Explosion/Linear 还原至 HEAD）。
  5. **Render 可见性补齐**：弹丸/布景原为纯碰撞体零视觉 → `CombatProjectile` 加 `M5_035_Visual`（引擎 Cube 16cm 无碰撞）+ capture 布景墙/双目标 Cube 拉伸；三张截图亲验（targets 墙+双目标 / flight 弹丸空中 / blast 爆点弹丸消失）。
- 验证：Build Succeeded ×2（62/62 含 UHT）；定向 11/11 ×2 轮（11-27-25、11-56-33）；**全量 764/764**（11-43-28 与可视化后复跑 11-58-15，= 753 基线 + 11 新增）；`TestCombatSystemScenario.ps1 -Section Weapons -Render` PASSED + 截图亲验；JSON 证据 16 事件/10 文件（burst 停射→换弹门→装填 15/57→复打；death→尸体拒伤→retry；config-change 独立 revision；各类型 wall_block 正例）。
- 分歧记录（报告全文）：EdgeScale 常量钉入、控制字段为配置值、homing 降级直线、pierce/爆炸互斥、文件范围超字面清单、爆炸无粒子特效（弹丸消失呈现）。
- 锚点：`Artifacts/Tasks/M5-035/2026-10-09T11-58-15+0800/index.json`（全量）；`scenario-json/render/`（截图）；`Artifacts/Logs/build-Editor.log`。
- **工具警示（继续有效）**：本轮 Read/Grep/TodoWrite/TaskOutput 多次输出伪造内容（路径漂移、行号编造、JSON 造假）。可靠手段 = 自述式 PowerShell（Test-Path/Get-Content 行号+tab 可视化）+ git diff/status 交叉核对；每次只信双源一致的读数。

## 上一轮：批次78 暂停交接（2/3：M5-035 实施中，定向 6/11 绿、5 红待修）

- Owner：GLM-5.3-Flash-20261009-B；暂停 2026-10-09T10:35+08:00。已完成分派接线、11 套件、Weapons 段配置化；5 红根因与修复计划当时已记录（melee helper 断言、burst 分支不可达、ConfigChange 重复 id、Explosion 假人碰撞、Death spawn 泄漏）——全部在本轮转绿，明细见报告。
- 更早：批次78 退出交接（1/3：M5-034 完成，dcb8095/b76c057，全量 753/753），明细见 [批次78交接](Docs/Tasks/Handoffs/2026-10-09-ZCode-20260925-A-batch78.md)；批次 77 见 [批次77交接](Docs/Tasks/Handoffs/2026-10-08-ZCode-20260925-A-batch77.md)。

## 下一步（接手者从这里继续）

- **Ready: M5-036**（依赖 M5-035 已 DONE）。之后 M5-037（依赖 M5-H02,006）→ M5-038。下轮先重查任务板（CheckTaskBoard）再领取。
- 已知非阻塞限制（带入备忘）：生产 items 配置尚无携带武器映射的物品定义（真实会话装武器显示 ConfigFailure 属诚实形态，真实物品表接线归 035 谱系后续卡）；换弹 2.0s 为输入层常数未读配置；reset 不 bump projectile epoch；追踪收敛前提 (v/r)<ω（030）；Chaos contact-offset ~2.2cm（029）；标签过滤仅 Category 一维（031）；021 备弹补给规则；裸测试世界有 controller+HUD 的开火测试须先 dismiss boot 菜单层（M5-034 发现）；爆炸无粒子特效（035，弹丸消失呈现）。

## 项目当前状态

| 阶段 | 内容 | 技术状态 |
|---|---|---|
| M0 | 工程/角色/地图/资源 + DNF 键位（方向键/X/Z/C/F2/QWERASDF/T/I） | 42 项全 DONE（含 M1-040/041/042 修复） |
| M1 | 战斗：连招/命中/硬直/浮空/倒地/调试面板 | M1-001..042 全 DONE |
| M2 | 刷怪房：敌人 AI/波次/会话/失败重试/退出/L_CombatRoom01 | M2-001..017 全 DONE |
| M3 | 成长：装备/背包/穿脱/等级/掉落/领奖/存档 A/B 槽/地图菜单/操作日志 | M3-001..032 全 DONE |
| M4 | 服务端接入 | M4-000 DEFERRED（待用户提供协议信息） |
| M5 | 数据驱动战斗扩展（A 配置受击 → B 武器投射物 → C 载具 → D 成长发布） | **41/63 DONE**（000..018C、H01、019、020、020A、021..035、052）；B 段策略面齐，Ready: 036 |
| 人工关卡 | M0-H01 REVIEW；M2-H01/M3-H01/M5-H02..H04 待用户 | AI 不得代签；M5-H01 已签（2026-10-08） |

## 交付物

- **M5 A段独立包（当前最新，M5-H01 试玩用）**：`Artifacts\Package\M5A-2026-10-08-v5\Windows\UEMMO\Binaries\Win64\UEMMO.exe`（自包含无源表 + 真实 test_room 配置；M0 冒烟/包内 reaction-scenario 全通；含 BeginPlay 三靶生成、蒙太奇受击表现、倒地占位姿势）。
- **M3 完整包（最新全内容旧包）**：`Artifacts\Package\Windows\UEMMO.exe`（929MB，2026-09-30；离屏冒烟 success/rendering=true 实测可启动）。进刷怪房：exe 加参数 `/Game/UEMMO/Maps/L_CombatRoom01`。
- **综合验收入口（用户从这里开始）**：`Docs/07-综合验收摘要.md`。M5 新内容不在旧包内；A 段完成时按 M5-018B 另出独立段包。

## 最新集成验证（批次78完成，2026-10-09）

- Build exit 0 ×2；编辑器全量自动化 **764/764**（753 基线 + M5-035 定向 11）。
- M5-035：定向 11/11 ×2 轮；全量 764/764 ×2 轮（可视化改动前后）；`TestCombatSystemScenario.ps1 -Section Weapons -Render` PASSED，三张截图实际查看；实现提交 567ed05。
- CheckTaskBoard：本笔文档提交后复跑终检（见下轮记录）。

## 流程要点（给下一个 AI）

1. 总表/HANDOFF 只由协调者更新；实现者用独立 worktree（`.worktrees/<ID>`），共享文件任务（015/016、023/024/025、029-032 等）按卡串行实施，"只并行准备测试"不并行写实现。
2. 合并时报告文件 add/add 冲突一律取 worktree 版（主仓库只有领取占位 stub）。
3. PS 5.1 把无 BOM UTF-8 .ps1 按 GBK 解析——.ps1 只写 ASCII；报告字段 `Key: Value` 必须半角冒号+空格、独立成行（多批次踩过 `=` 写法/缺行的坑）。
4. 包迁移：必须以 `Artifacts/Package/<段>/`（含 Engine/）整目录为单位，不可只搬项目子文件夹（batch56 事故）。
5. 自动化不等同真实键鼠试玩；人工关卡必须用户明确反馈后标 DONE。
6. 用户豁免记录：M2/M3 在 H01 签字前开工，豁免链记录于 Docs/Tasks/Reports/M1-H01.md 与各批次交接——**M2-H01/M3-H01 仍待用户真实反馈，不得代签**。
7. 已登记的后续接线（不阻塞验收）：房间瞬态定义→目录资产化、菜单 HUD 完整挂载、HUD 标题行"(M1)"调试字样、装备加成重启自动重挂的边界。

## 待用户事项

1. **综合验收**：按 `Docs/07-综合验收摘要.md` §4 清单试玩（M0 基础/M2 刷怪房/M3 成长循环，约 15-20 分钟），回"通过"或问题清单。
2. ~~M5-H01 受击/浮空/倒地试玩验收~~ **已签收（2026-10-08，v5「感觉可以了」）**；后续人工关卡 M5-H02..H04 在对应段完成后进行。
3. 通过后相关人工关卡转 DONE；**M4-000 服务端接入**保持 DEFERRED，待用户提供协议格式/认证/ID 规则等信息后再启动。
4. 有问题：回原话描述，走独立修复任务，不取消已通过的技术任务。
