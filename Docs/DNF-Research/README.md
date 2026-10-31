# DNF 逆向研究与 UE-MMO 扩展规划

本目录集中保存此前在 D:/DOF/DNF-REA-Output/docs 编写的 **11 份**研究与设计文档。文档中的既有统计是 2026-10-31 至 2026-11-01 的静态研究记录，不代表多职业功能已在 UE-MMO 实现。当前目标仅为本机私人学习试玩；DNF 参考图及其衍生内容不提交到仓库，也不作为可发行素材。

## 阅读顺序

1. [逆向研究总览](00-总览.md)、[技能](01-技能系统.md)、[关卡](02-关卡系统.md)、[战斗](03-战斗系统.md)、[职业](04-职业系统.md)及[美术清单](05-美术资源.md)。
2. [多职业、技能等级与连续两房间设计](06-UE-MMO多职业双房间扩展设计-待确认.md)和[首职业资源交接清单](07-首职业资源交接清单-待确认.md)。这些是规划，实施以 TASKS.md 的新任务卡为准。
3. [参考帧候选](08-Blender首轮参考候选-待确认.md)、[穿衣角色/怪物细化设计](09-首职业与怪物服装细化设计-待审阅.md)、[交给建模 Agent 的逐阶段计划](10-Blender建模Agent执行计划.md)。

## 素材位置和访问规则

- **项目内私有参考**：../../SourceAssets/Unpacked/DNFPrivateReference。该目录已被 .gitignore 忽略，保存建模图板、逐帧样本、少量原始 IMG/PVF 示例、必要的 CSV/JSON 索引、提取器源码以及每文件 SHA-256 清单。请勿强制添加到 Git、公开同步或放入 Content。
- **本机完整研究归档**：D:/DOF/DNF-REA-Output。这里才包含全部 NPK、IMG、PVF 和媒体文件。文档中未被迁入私有目录的相对名称，例如 pvf/ 全量、assets/raw/、assets/images/、code/squirrel/、reports/pvf-manifest.json 与 reports/img-files.jsonl，均指向此归档；不要误认为它们在 Git 仓库中。
- **原客户端只读输入**：D:/DOF/地下城与勇士。不要修改此目录。
- **Blender 可执行文件**：D:/DOF/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender.exe，已验证为 5.2.4 LTS Release Candidate。

## 已确认与待定

已确认多个独立角色、首职业六技能包含现有 Z 上挑、仅本机私人学习试玩、角色与怪物都要有完整衣服/护具。格斗家 + 哥布林是首轮造型方向；具体五个热栏技能、双房间的连续模式和跨角色物品共享仍以用户最终决定为准。当前 UE 任务板中的 M5-H03 人工试玩未由 AI 签收。

## 迁移校验

迁入过程可通过 Scripts/MigrateDNFResearchDocs.py 重跑；它从外部研究归档复制，先逐文件比对 SHA-256，再重写项目内文档链接并记录改写后摘要；不删除原件。私有目录的 migration-manifest.json 记录实际文件、最终哈希和链接改写摘要。这个脚本只负责文档与参考资料，不实施模型或游戏功能。
