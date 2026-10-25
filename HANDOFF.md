# 最近一轮交接：第六批（M5-078 收官）

- Owner：ZCode-20261024-A；2026-10-26T02:18+08:00 至 02:36+08:00。
- 本轮新增 DONE **1 项：M5-078**；任务板 **168/190 DONE、0 IN_PROGRESS、valid=true**；AI 可做卡**全部完成**（M5-067～078、M0-006）。分支 `main` 领先远程 33 提交，未推送。
- 提交：实现 `7b8a8ed`、文档 `18a8af1`、HANDOFF/索引 `（本批收尾提交）`。

## 本轮实际内容

**M5-078 投射物容量活跃核算（方案 a，用户以"继续"授权推荐方案）**：`FProjectileWorldService` 的容量门与 `GetNumLiveProjectiles` 改为按**活跃**（actor 存在且非 pending-kill）计量——退役弹体在 Destroy 当刻让出容量槽位，Destroy→GC 的陈旧窗口不再消耗 256 上限；默认 GC 设置下持续退役吞吐不再有 4.3 弹/s 的周期拒发边界；GC 行为零改动、不做对象池。新自由函数 `IsProjectileRecordActive` 承载语义（注释覆盖全部退役面）。验证：M5_050 容量语义回归 3/3（256 满载显式拒绝保持）、M5_027/028 6/6+6/6、全量 **805/805**。

**固定包已刷新至最终 HEAD**：`Artifacts/Package/Windows/package-info.json`——generated_at 2026-10-26T02:33:43+08:00、source_revision `18a8af1`（含 067-078 全部修复）、exe SHA256 `8E6736ED...6751`。

## 当前全局状态

- AI 可做任务清零；剩余 TODO（M5-037+ 载具/持久化链、M5-H03/H04）全部等 **M5-H02 用户试玩签收**（其又依赖 M0-H01）。
- `M5-H02` REVIEW：[试玩指引](Docs/Tasks/M5-H02-试玩指引.md)（包身份以最新 package-info 为准）；AI 不代签。
- 审核材料：[M5-适配链审核索引](Docs/Tasks/M5-适配链审核索引.md) 已含全部 11 张卡的 SHA 与重点断言。

## 下一步

等用户：① M5-H02 试玩逐项反馈（通过项/问题+原话）；② 之后按 [执行与交接规则](Docs/Tasks/执行与交接规则.md) §6 更新报告与总表，解锁 M5-037/M5-046 并行开工。AI 侧无待办。
