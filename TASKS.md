# UE-MMO 任务总表

这是本项目**唯一的任务状态来源**。任务卡描述做什么；Reports 留证据；HANDOFF 描述最近一轮；均不维护第二份进度表。

## 使用入口

1. 先读 [执行与交接规则](Docs/Tasks/执行与交接规则.md)、[接口约定](Docs/Tasks/接口约定.md)、[HANDOFF](HANDOFF.md)。
2. 运行只读检查：`rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/CheckTaskBoard.ps1`。
3. 选择最前一个依赖全部 DONE 的 TODO 任务，读对应任务卡，一次只领取一项。
4. **每完成 1 项立即更新本表和独立报告；每完成 3 项写批次交接；不足 3 项退出也必须交接。**
5. 完成数量以检查脚本实时统计为准，不凭聊天记录或手工百分比。M0 是既有基线，不计入本轮 AI 新完成任务数。

## 状态与范围

`TODO` 未领取；`IN_PROGRESS` 正在做；`PAUSED` 已交接可续做；`BLOCKED` 依赖外部条件；`REVIEW` 等待人工验收；`DONE` 验收通过；`DEFERRED` 不在当前执行范围；`SUPERSEDED` 被明确子任务替代。依赖未完成的 TODO 无须改 BLOCKED。

当前默认执行范围到 M3 单机完整循环；M1/M2/M3 阶段末人工验收各自通过后才进入下一阶段。M4 服务器接入未获得接口信息，保持 DEFERRED。

Owner 使用工具与会话标识（例如 ZCode-20260925-A），不是机器用户名。更新时间用带时区 ISO 8601。历史 M0 行时间是本次导入总表时间，实际证据见原验证记录。

## 总表

| ID | 任务 | 状态 | 依赖 | Owner | 更新时间 | 报告/证据 |
|---|---|---|---|---|---|---|
| M0-001 | UE 工程与兼容工具链 | DONE | — | 既有基线 | 2026-09-25T06:05:19+08:00 | [M0-001](Docs/Tasks/Reports/M0-001.md) |
| M0-002 | 现成模型、动画、音效入库 | DONE | — | 既有基线 | 2026-09-25T06:05:19+08:00 | [M0-002](Docs/Tasks/Reports/M0-002.md) |
| M0-003 | 横版移动与测试房间 | DONE | — | 既有基线 | 2026-09-25T06:05:19+08:00 | [M0-003](Docs/Tasks/Reports/M0-003.md) |
| M0-004 | 构建、运行与独立包验证 | DONE | — | 既有基线 | 2026-09-25T06:05:19+08:00 | [M0-004](Docs/Tasks/Reports/M0-004.md) |
| M0-005 | 设计文档与 Agent 准备 | DONE | — | 既有基线 | 2026-09-25T06:05:19+08:00 | [M0-005](Docs/Tasks/Reports/M0-005.md) |
| M0-H01 | [真实键盘与镜头验收](Docs/Tasks/M0-H01.md) | REVIEW | M0-003,M0-004 | 用户 | 2026-09-25T06:05:19+08:00 | [待试玩记录](Docs/Tasks/Reports/M0-H01.md) |
| M1-001 | [接手并复核现有基线](Docs/Tasks/M1-001.md) | DONE | M0-004 | ZCode-20260925-A | 2026-09-25T10:11:53+08:00 | [report](Docs/Tasks/Reports/M1-001.md) |
| M1-002 | [建立按任务筛选的自动化测试入口](Docs/Tasks/M1-002.md) | DONE | M1-001 | ZCode-20260925-A | 2026-09-25T23:14:22+08:00 | [report](Docs/Tasks/Reports/M1-002.md) |
| M1-003 | [输入缓存的容量与一次性消费](Docs/Tasks/M1-003.md) | DONE | M1-002 | ZCode-20260925-A | 2026-09-25T23:20:30+08:00 | [report](Docs/Tasks/Reports/M1-003.md) |
| M1-004 | [输入过期与复位清理](Docs/Tasks/M1-004.md) | DONE | M1-003 | ZCode-20260925-A | 2026-09-25T23:46:41+08:00 | [report](Docs/Tasks/Reports/M1-004.md) |
| M1-005 | [半开窗口与跨帧检测](Docs/Tasks/M1-005.md) | DONE | M1-002 | ZCode-20260925-A | 2026-09-25T23:46:42+08:00 | [report](Docs/Tasks/Reports/M1-005.md) |
| M1-006 | [固定60Hz动作时钟](Docs/Tasks/M1-006.md) | DONE | M1-005 | ZCode-20260925-A | 2026-09-26T00:15:43+08:00 | [report](Docs/Tasks/Reports/M1-006.md) |
| M1-007 | [攻击数据类型与单条校验](Docs/Tasks/M1-007.md) | DONE | M1-002 | ZCode-20260925-A | 2026-09-26T00:15:44+08:00 | [report](Docs/Tasks/Reports/M1-007.md) |
| M1-008 | [四个攻击的文本数据与引用校验](Docs/Tasks/M1-008.md) | DONE | M1-007 | ZCode-20260925-A | 2026-09-26T00:49:32+08:00 | [report](Docs/Tasks/Reports/M1-008.md) |
| M1-009 | [从JSON生成攻击DataAsset](Docs/Tasks/M1-009.md) | DONE | M1-008 | ZCode-20260925-A | 2026-09-26T02:08:05+08:00 | [report](Docs/Tasks/Reports/M1-009.md) |
| M1-010 | [运行时攻击目录与打包引用](Docs/Tasks/M1-010.md) | DONE | M1-009 | ZCode-20260925-A | 2026-09-26T02:32:26+08:00 | [report](Docs/Tasks/Reports/M1-010.md) |
| M1-011 | [单次攻击开始、推进与结束](Docs/Tasks/M1-011.md) | DONE | M1-004,M1-006,M1-010 | ZCode-20260925-A | 2026-09-26T02:51:15+08:00 | [report](Docs/Tasks/Reports/M1-011.md) |
| M1-012 | [J与K输入转成战斗意图](Docs/Tasks/M1-012.md) | DONE | M1-011 | ZCode-20260925-A | 2026-09-26T03:13:47+08:00 | [report](Docs/Tasks/Reports/M1-012.md) |
| M1-013 | [攻击期间移动和朝向限制](Docs/Tasks/M1-013.md) | DONE | M1-012 | ZCode-20260925-A | 2026-09-26T03:31:09+08:00 | [report](Docs/Tasks/Reports/M1-013.md) |
| M1-014 | [普攻两段的缓存衔接](Docs/Tasks/M1-014.md) | DONE | M1-013 | ZCode-20260925-A | 2026-09-26T03:52:08+08:00 | [report](Docs/Tasks/Reports/M1-014.md) |
| M1-015 | [生命值扣除与死亡单次事件](Docs/Tasks/M1-015.md) | DONE | M1-002 | ZCode-20260925-A | 2026-09-25T23:46:43+08:00 | [report](Docs/Tasks/Reports/M1-015.md) |
| M1-016 | [可受伤的训练敌人](Docs/Tasks/M1-016.md) | DONE | M1-015 | ZCode-20260925-A | 2026-09-26T00:49:33+08:00 | [report](Docs/Tasks/Reports/M1-016.md) |
| M1-017 | [命中盒的世界坐标计算](Docs/Tasks/M1-017.md) | DONE | M1-013 | ZCode-20260925-A | 2026-09-26T09:45:53+08:00 | [report](Docs/Tasks/Reports/M1-017.md) |
| M1-018 | [三维命中查询与过滤](Docs/Tasks/M1-018.md) | DONE | M1-016,M1-017 | ZCode-20260925-A | 2026-09-26T10:45:04+08:00 | [report](Docs/Tasks/Reports/M1-018.md) |
| M1-019 | [有效窗口扣血与同招去重](Docs/Tasks/M1-019.md) | DONE | M1-018,M1-011 | ZCode-20260925-A | 2026-09-26T11:12:19+08:00 | [report](Docs/Tasks/Reports/M1-019.md) |
| M1-020 | [受击硬直打断攻击](Docs/Tasks/M1-020.md) | DONE | M1-019 | ZCode-20260925-A | 2026-09-26T12:06:15+08:00 | [report](Docs/Tasks/Reports/M1-020.md) |
| M1-021 | [上挑输入和取消窗口](Docs/Tasks/M1-021.md) | DONE | M1-014,M1-020 | ZCode-20260925-A | 2026-09-26T12:41:56+08:00 | [report](Docs/Tasks/Reports/M1-021.md) |
| M1-022 | [上挑命中产生物理浮空](Docs/Tasks/M1-022.md) | DONE | M1-021 | ZCode-20260925-A | 2026-09-26T13:35:42+08:00 | [report](Docs/Tasks/Reports/M1-022.md) |
| M1-023 | [上挑后的跳跃取消](Docs/Tasks/M1-023.md) | DONE | M1-021 | ZCode-20260925-A | 2026-09-26T14:15:51+08:00 | [report](Docs/Tasks/Reports/M1-023.md) |
| M1-024 | [空中普攻路由与追击](Docs/Tasks/M1-024.md) | DONE | M1-022,M1-023 | ZCode-20260925-A | 2026-09-26T14:48:29+08:00 | [report](Docs/Tasks/Reports/M1-024.md) |
| M1-025 | [浮空次数限制与冲量衰减](Docs/Tasks/M1-025.md) | DONE | M1-024 | ZCode-20260925-A | 2026-09-26T15:23:32+08:00 | [report](Docs/Tasks/Reports/M1-025.md) |
| M1-026 | [落地、倒地与恢复](Docs/Tasks/M1-026.md) | DONE | M1-025 | ZCode-20260925-A | 2026-09-26T16:00:26+08:00 | [report](Docs/Tasks/Reports/M1-026.md) |
| M1-027 | [训练场一键复位所有战斗状态](Docs/Tasks/M1-027.md) | DONE | M1-026 | ZCode-20260925-A | 2026-09-26T16:33:17+08:00 | [report](Docs/Tasks/Reports/M1-027.md) |
| M1-028 | [战斗调试覆盖层](Docs/Tasks/M1-028.md) | DONE | M1-027 | ZCode-20260925-A | 2026-09-26T17:37:09+08:00 | [report](Docs/Tasks/Reports/M1-028.md) |
| M1-029 | [分方向速度和斜向归一化](Docs/Tasks/M1-029.md) | DONE | M1-002 | ZCode-20260925-A | 2026-09-26T00:15:45+08:00 | [report](Docs/Tasks/Reports/M1-029.md) |
| M1-030 | [镜头跟随地面锚点](Docs/Tasks/M1-030.md) | DONE | M1-029 | ZCode-20260925-A | 2026-09-26T00:49:33+08:00 | [report](Docs/Tasks/Reports/M1-030.md) |
| M1-031 | [八方向移动与跳跃动画层](Docs/Tasks/M1-031.md) | DONE | M1-029 | ZCode-20260925-A | 2026-09-26T02:08:06+08:00 | [report](Docs/Tasks/Reports/M1-031.md) |
| M1-032 | [攻击Montage与动画所有权](Docs/Tasks/M1-032.md) | DONE | M1-014,M1-031 | ZCode-20260925-A | 2026-09-26T10:45:02+08:00 | [report](Docs/Tasks/Reports/M1-032.md) |
| M1-033 | [局部命中停顿与输入保留](Docs/Tasks/M1-033.md) | DONE | M1-032,M1-026 | ZCode-20260925-A | 2026-09-26T19:28:29+08:00 | [report](Docs/Tasks/Reports/M1-033.md) |
| M1-034 | [拳击和落地音效事件](Docs/Tasks/M1-034.md) | DONE | M1-019,M1-032 | ZCode-20260925-A | 2026-09-26T12:41:57+08:00 | [report](Docs/Tasks/Reports/M1-034.md) |
| M1-035 | [生命条与命中数字反馈](Docs/Tasks/M1-035.md) | DONE | M1-028,M1-034 | ZCode-20260925-A | 2026-09-26T20:11:36+08:00 | [report](Docs/Tasks/Reports/M1-035.md) |
| M1-036 | [上挑与空中动作素材验收记录](Docs/Tasks/M1-036.md) | DONE | M1-032,M1-026 | ZCode-20260925-A | 2026-09-26T17:39:05+08:00 | [report](Docs/Tasks/Reports/M1-036.md) |
| M1-037 | [完整连招的可重复场景测试](Docs/Tasks/M1-037.md) | DONE | M1-027,M1-030,M1-033,M1-035,M1-036 | ZCode-20260925-A | 2026-09-26T21:11:47+08:00 | [report](Docs/Tasks/Reports/M1-037.md) |
| M1-038 | [帧率与卡顿边界回归](Docs/Tasks/M1-038.md) | DONE | M1-037 | ZCode-20260925-A | 2026-09-26T22:18:15+08:00 | [report](Docs/Tasks/Reports/M1-038.md) |
| M1-039 | [M1独立包与交接验收包](Docs/Tasks/M1-039.md) | DONE | M1-038 | ZCode-20260925-A | 2026-09-26T23:16:35+08:00 | [report](Docs/Tasks/Reports/M1-039.md) |
| M1-040 | [DNF键位映射](Docs/Tasks/M1-040.md) | DONE | M1-012 | ZCode-20260925-A | 2026-09-27T01:35:43+08:00 | [report](Docs/Tasks/Reports/M1-040.md) |
| M1-042 | [HUD标题阶段文案清理](Docs/Tasks/M1-042.md) | DONE | M1-040 | ZCode-20260925-A | 2026-09-27T03:04:33+08:00 | [report](Docs/Tasks/Reports/M1-042.md) |
| M1-043 | [敌侧战斗组件游戏驱动](Docs/Tasks/M1-043.md) | DONE | M1-041 | ZCode-20260925-A | 2026-09-27T03:30:27+08:00 | [report](Docs/Tasks/Reports/M1-043.md) |
| M1-H01 | [M1 用户阶段验收](Docs/Tasks/M1-H01.md) | BLOCKED | M1-039,M0-H01 | 用户 | 2026-09-27T03:30:30+08:00 | [report](Docs/Tasks/Reports/M1-H01.md) |
| M1-041 | [游戏侧战斗接线](Docs/Tasks/M1-041.md) | DONE | M1-012,M1-021,M1-040 | ZCode-20260925-A | 2026-09-27T03:04:31+08:00 | [report](Docs/Tasks/Reports/M1-041.md) |
| M2-001 | [近战敌人配置与出生冷却](Docs/Tasks/M2-001.md) | DONE | M1-039 | ZCode-20260925-A | 2026-09-27T03:55:52+08:00 | [report](Docs/Tasks/Reports/M2-001.md) |
| M2-002 | [追击与纵深对齐](Docs/Tasks/M2-002.md) | DONE | M2-001 | ZCode-20260925-A | 2026-09-27T04:36:09+08:00 | [report](Docs/Tasks/Reports/M2-002.md) |
| M2-003 | [敌人攻击前摇和恢复](Docs/Tasks/M2-003.md) | DONE | M2-002 | ZCode-20260925-A | 2026-09-27T05:32:08+08:00 | [report](Docs/Tasks/Reports/M2-003.md) |
| M2-004 | [玩家受击、死亡与控制释放](Docs/Tasks/M2-004.md) | DONE | M2-003 | ZCode-20260925-A | 2026-09-27T06:07:33+08:00 | [report](Docs/Tasks/Reports/M2-004.md) |
| M2-005 | [房间定义与边界校验](Docs/Tasks/M2-005.md) | DONE | M2-001 | ZCode-20260925-A | 2026-09-27T04:36:11+08:00 | [report](Docs/Tasks/Reports/M2-005.md) |
| M2-006 | [单局会话状态和唯一标识](Docs/Tasks/M2-006.md) | DONE | M2-005 | ZCode-20260925-A | 2026-09-27T05:32:10+08:00 | [report](Docs/Tasks/Reports/M2-006.md) |
| M2-007 | [可取消的单波刷怪器](Docs/Tasks/M2-007.md) | DONE | M2-006,M2-004 | ZCode-20260925-A | 2026-09-27T09:47:41+08:00 | [report](Docs/Tasks/Reports/M2-007.md) |
| M2-008 | [死亡驱动波次推进](Docs/Tasks/M2-008.md) | DONE | M2-007 | ZCode-20260925-A | 2026-09-27T10:39:28+08:00 | [report](Docs/Tasks/Reports/M2-008.md) |
| M2-009 | [进房后激活与出口状态](Docs/Tasks/M2-009.md) | DONE | M2-008 | ZCode-20260925-A | 2026-09-27T11:37:31+08:00 | [report](Docs/Tasks/Reports/M2-009.md) |
| M2-010 | [玩家失败与单局重试](Docs/Tasks/M2-010.md) | DONE | M2-009 | ZCode-20260925-A | 2026-09-27T12:10:31+08:00 | [report](Docs/Tasks/Reports/M2-010.md) |
| M2-011 | [退出房间和地图卸载清理](Docs/Tasks/M2-011.md) | IN_PROGRESS | M2-010 | ZCode-20260925-A | 2026-09-27T12:10:36+08:00 | [report](Docs/Tasks/Reports/M2-011.md) |
| M2-012 | [胜利失败界面和重试按钮](Docs/Tasks/M2-012.md) | TODO | M2-011 | — | — | — |
| M2-013 | [单局结果与结算一次性标识](Docs/Tasks/M2-013.md) | TODO | M2-012 | — | — | — |
| M2-014 | [两波刷怪完整场景回归](Docs/Tasks/M2-014.md) | TODO | M2-013 | — | — | — |
| M2-015 | [M2独立包与用户试玩包](Docs/Tasks/M2-015.md) | TODO | M2-014 | — | — | — |
| M2-H01 | [M2 用户阶段验收](Docs/Tasks/M2-H01.md) | TODO | M2-015 | 用户 | — | — |
| M3-001 | [装备定义和稳定实例ID](Docs/Tasks/M3-001.md) | TODO | M2-H01 | — | — | — |
| M3-002 | [30格背包增删与容量](Docs/Tasks/M3-002.md) | TODO | M3-001 | — | — | — |
| M3-003 | [本地角色Profile与新游戏初值](Docs/Tasks/M3-003.md) | TODO | M3-002 | — | — | — |
| M3-004 | [装备槽约束与穿脱](Docs/Tasks/M3-004.md) | TODO | M3-003 | — | — | — |
| M3-005 | [从装备重算最终属性](Docs/Tasks/M3-005.md) | TODO | M3-004 | — | — | — |
| M3-006 | [等级经验曲线与升级](Docs/Tasks/M3-006.md) | TODO | M3-003 | — | — | — |
| M3-007 | [固定种子的掉落表](Docs/Tasks/M3-007.md) | TODO | M3-001 | — | — | — |
| M3-008 | [结算奖励草稿与幂等申请](Docs/Tasks/M3-008.md) | TODO | M3-003,M3-006,M3-007,M2-013 | — | — | — |
| M3-009 | [满包时保留待领取奖励](Docs/Tasks/M3-009.md) | TODO | M3-008 | — | — | — |
| M3-010 | [装备与成长接入本地战斗属性](Docs/Tasks/M3-010.md) | TODO | M3-005,M3-006 | — | — | — |
| M3-011 | [背包只读列表界面](Docs/Tasks/M3-011.md) | TODO | M3-002,M3-003 | — | — | — |
| M3-012 | [界面穿脱装备与属性对比](Docs/Tasks/M3-012.md) | TODO | M3-011,M3-004,M3-005 | — | — | — |
| M3-013 | [版本化存档结构与往返序列化](Docs/Tasks/M3-013.md) | TODO | M3-003,M3-009,M3-004,M3-006 | — | — | — |
| M3-014 | [A/B双槽保存与索引提交](Docs/Tasks/M3-014.md) | TODO | M3-013 | — | — | — |
| M3-015 | [启动加载与损坏回退](Docs/Tasks/M3-015.md) | TODO | M3-014 | — | — | — |
| M3-016 | [奖励与存档的原子提交](Docs/Tasks/M3-016.md) | TODO | M3-015,M3-009 | — | — | — |
| M3-017 | [单图选择菜单与进出副本](Docs/Tasks/M3-017.md) | TODO | M3-015,M2-011 | — | — | — |
| M3-018 | [结算奖励界面与待领取提示](Docs/Tasks/M3-018.md) | TODO | M3-016,M3-017,M2-012 | — | — | — |
| M3-019 | [三种装备图标与显示资源登记](Docs/Tasks/M3-019.md) | TODO | M3-018 | — | — | — |
| M3-020 | [单机成长循环集成测试](Docs/Tasks/M3-020.md) | TODO | M3-019,M3-010 | — | — | — |
| M3-021 | [保存恢复和重复领取故障回归](Docs/Tasks/M3-021.md) | TODO | M3-020 | — | — | — |
| M3-022 | [单机原型发布包与完整交接](Docs/Tasks/M3-022.md) | TODO | M3-021 | — | — | — |
| M3-H01 | [M3 用户阶段验收](Docs/Tasks/M3-H01.md) | TODO | M3-022 | 用户 | — | — |
| M4-000 | [后续服务端接入范围](Docs/Tasks/M4-000.md) | DEFERRED | M3-H01 | — | — | — |
