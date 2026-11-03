# 给后续 Agent 的 DNFM 资源接手入口

更新：2026-11-04。用户已要求当前会话将所有已提取的3D模型、地图网格和相关必要素材放入本项目，由用户另行安排其他 Agent 实施后续任务。本文件是操作入口，任务状态仍以根目录 TASKS.md 为准。

## 先从这里打开

工程：`D:\Github-Poj\UE-MMO\UEMMO.uproject`，引擎 UE **5.8.3**。

UE 内容浏览器根路径：`/Game/UEMMO/Imported/DNFM130_5_8`。

| 内容 | UE 位置 / 本地位置 | 数量与状态 |
|---|---|---|
| 全部模型 | `/Game/UEMMO/Imported/DNFM130_5_8/Meshes` | **279 个 Static Mesh**，已导入保存并用新UE进程重载 |
| 地图/背景网格 | 同一 Meshes 目录，查 `map_mesh_catalog.json` | **183 个**：153 个公会场景件 + 30 个背景件，属于279之内，不额外复制 |
| 其他模型 | 同一 Meshes 目录，查 `asset_catalog.csv` 的 category | 43 UI网格、40 FX网格、13Unity内置几何体 |
| 来源材质 | `/Game/UEMMO/Imported/DNFM130_5_8/Materials` | 137 个来源材质 + 2 个辅助材质 |
| 模型关联可用贴图 | `/Game/UEMMO/Imported/DNFM130_5_8/Textures` | 39 张；原Repeat/Clamp和点采样设置已适配 |
| 全量展示图 | `/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_ResourceGallery` | 已保存的独立展示场景，不是重建后的原游戏地图 |
| FBX + 原几何字段 | `SourceAssets/Unpacked/DNFM130_5_8/fbx`、`geometry` | 279 份FBX及原几何JSON，保留UV/顶点色等 |
| 原始导航 | `SourceAssets/Unpacked/DNFM130_5_8/handoff/map_support` | 2份guildagit/myhome导航原件，尚未转换成UE导航网格 |

“地图网格”已包含在279个模型中，并非未导入的另一批。不要因为目录里没有单独的“地图模型”文件夹就重复导入或复制同名资产。

## 本机交接包

根目录：`SourceAssets/Unpacked/DNFM130_5_8/handoff/`。文件中的 `project_file`、`fbx`、`source_copies` 等路径均相对于 UE-MMO 项目根目录。

| 文件/目录 | 用途 |
|---|---|
| `asset_catalog.json` | 全部279模型、137材质、39纹理的机器可读索引；含真实 UE object_path、材质变体、原始对象键、可用纹理和缺失项 |
| `asset_catalog.csv` | 便于按名称、category、UE路径、外观状态筛选的逐模型索引 |
| `map_mesh_catalog.json` | 仅183地图/背景网格的完整子集索引，加2份导航来源 |
| `missing_dependencies.json` | 154个模型默认材质缺图情况；精确缺失材质/纹理对象键和56个外部CAB引用边界 |
| `source_files.json` | 原始文件副本、原包名、源路径、SHA-256、大小及可解析依赖边；用于追溯 |
| `original_sources/` | **297份**可获得的原Unity数据/Bundle依赖，约45.9MB，全部副本与来源哈希一致 |
| `map_support/` | 两个原始导航二进制，保持原字节 |
| `verification.json` | 接手包本轮结构和副本校验结果 |

`original_sources` 从涉及模型的 renderer/material/texture/shader 包开始，沿可解析的外部CAB关系补入当前首包中存在的依赖。56个缺失CAB是这个依赖范围内的缺口，不能与154个缺图模型数量相互替换；并非每个缺失CAB都用于当前默认材质。不要创建空文件伪装补齐。

这些原始依赖现在位于UE-MMO内部，后续检查网格/材质/prefab时不用再凭聊天记录定位DNFM文件。若要重做从APK到全量资源导出的全过程，完整APK、解包清单仍在 `D:\Github-Poj\DNFM`；接手包没有复制整份APK或全部8,445个Bundle。

## 后续 Agent 的直接执行步骤

1. 先读项目 `AGENTS.md`、`TASKS.md`、`HANDOFF.md`；按用户新指定的任务范围领取任务，不自动领取或代签人工关。
2. 用 `asset_catalog.csv` 按名称/类别筛选资产，或者在UE中打开上面的展示图。CSV的 `ue_object_path` 可直接用于 `unreal.load_asset()`。
3. 查看资产行的 `appearance`、`missing_texture_refs`、`material_variants`。缺图物件与特殊着色器近似必须在任务方案中计入，不把灰色占位认作最终美术。
4. 拖入资产到自己的新地图/Blueprint即可使用几何体。需要原prefab层级时，按 `source_copies` 打开项目本地原Bundle追溯；索引不提供已组装的原地图坐标。
5. 材质变体在 `material_variants` 内有明确UE资产路径。默认选取优先实际跨包prefab MeshRenderer；不同用途应根据具体prefab选择，不按文件名猜材质。
6. 保存后用新进程重载验证；改视觉资源需要渲染并查看图片。不要同时启动两个写本工程资源的UE进程。

可以给后续 Agent 的任务提示：

> 在 D:\Github-Poj\UE-MMO 中执行我指定的后续任务。先读 AGENTS.md、TASKS.md、HANDOFF.md 和 Docs/DNF-Research/06-Agent资源接手入口.md。DNFM 的279个静态模型（含183个地图/背景网格）已导入 /Game/UEMMO/Imported/DNFM130_5_8，先通过 SourceAssets/Unpacked/DNFM130_5_8/handoff/asset_catalog.json 选择现有资产并检查缺失项。只做我指定的功能，不重复导入全部资产，不把缺失纹理或近似Shader当作已完整还原。

用户负责分配其他 Agent；本次没有给其他会话发消息或创建后续工作会话。

## 必须保留的实际限制

- **154个模型默认材质仍缺源贴图**，已设置明确的中性占位。公会36个底色材质的主要缺口是 `CAB-6af7f14628a7155654ed172c566e3677`；当前APK的主包与外层Unity资源都找不到。只有获取对应更新包/源贴图后才能恢复原色。
- Unity特殊Shader只做近似映射，未实现的扭曲、溶解、描边等输入记录在材质metadata `DNFM.UnimplementedTextureInputs` 以及源参数。源模型具备UV/法线/顶点色，不等于原运行时外观完全等价。
- 共52个源网格带颜色，UE保留50个非白颜色缓冲，另2个全白可省略；25个保留第二UV。279FBX读回和UE重载已验证。3D角色骨骼和原动作不在这279个网格中。
- 源坐标按1单位=1米转UE厘米，展示Actor缩放用于方便查看，不是原地图最终实例大小；使用时结合目标关卡尺寸设置。
- 导航文件是原NRBF/VAND数据；项目地图要在UE生成自己的NavMesh。既有材料不能直接产生原地图、战斗逻辑、人物换装或多人系统。
- 静态网格碰撞为Complex As Simple供静态查询，展示Actor关闭碰撞；如需物理模拟应建立简单碰撞。
- 资产和素材是本机Git忽略数据，不会随普通Git拉取自动出现。在另一台机器接手时需同时复制 `Content/UEMMO/Imported/DNFM130_5_8` 和 `SourceAssets/Unpacked/DNFM130_5_8`。当前电脑直接使用即可。

## 检查与重跑

检查现有UE资产（不重新导入、不改现有网格）：

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMImport.ps1 -Mode verify
```

重新生成接手索引和依赖副本（需要完整原始导出目录及现有跨包扫描证据）：

```powershell
rtk proxy py Scripts/package_dnfm_handoff.py
```

从原数据重新做FBX/UE导入的完整顺序见 [05-DNFM静态资源UE适配](05-DNFM静态资源UE适配.md)。导入/关卡生成脚本会更新该脚本所有的目标资产；用户已改过的资产应先另存。无需为了开始场景制作再跑一遍整批导入。

## 证据

本轮任务 [M6-010](../Tasks/M6-010.md)，报告 [M6-010报告](../Tasks/Reports/M6-010.md)。本轮新进程检查再次通过279模型、137来源材质、39纹理；原始副本297份和导航2份全部校验。

M6-009已经完成实际展示渲染和项目Test -Render。本轮只核对现有uasset并补充源副本和索引，未改几何/材质，也没有把历史截图说成本轮新渲染。
