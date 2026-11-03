# DNFM 130.5.8.0 静态资源适配

日期：2026-11-04。任务：[M6-009](../Tasks/M6-009.md)。用户在当前会话确认资源为自己的项目，明确要求迁入 `D:\Github-Poj\UE-MMO`，本轮不以性能优化为目标。

后续Agent可直接阅读[06-资源接手入口](06-Agent资源接手入口.md)。全部模型已核实在项目内，另已补齐297份可用原始依赖副本、2份导航原件及279模型/183地图网格的可查询索引。

## 在项目中打开

内容浏览器路径：`/Game/UEMMO/Imported/DNFM130_5_8`。

- `Meshes`：279 个 Static Mesh（266 个原包网格、13 个 Unity 内置几何体）。可直接拖入自己的关卡。
- `Materials`：137 个来源材质的 UE 近似转换；另外有无材质对象与展示地板的辅助材质。
- `Textures`：按实际材质指针找到的 39 个纹理。保留源 Repeat/Clamp、必要的点采样设置。
- `Maps/L_DNFM_ResourceGallery`：全部网格的独立展示场景，Actor 带原名称；展示用缩放使小件和大件都能查看，资产本身保留统一单位转换。

导入资产是本机真实 `.uasset`/`.umap`，通过 UE 5.8.3 编辑器 API 创建并保存。生成目录、FBX、Blender 工程和中间资源保留在本机 Git 忽略目录；脚本与说明可追溯。当前没有自动替换既有角色、战斗逻辑或地图主流程。

## 可用性边界

279 个网格包含 UV 与法线；52 个含顶点色，25 个含第二套 UV（其中 21 个来自主包，其余是内置几何体）。FBX 重新读取原 Bundle 几何与通道，因此不是只对先前 OBJ 改后缀。顶点色字节归一化到 0–1，并使用 LINEAR FBX 色彩约定验证。

原数值单位按 1 Unity 单位 = 1 米转换到 UE 厘米，Unity `(x,y,z)` → Blender `(x,-z,y)`，FBX 再负责引擎坐标约定转换；没有从任意纹理或名称猜测真实建筑大小。原 prefab 的嵌套 Transform 没有被重建为原版地图，独立静态网格的数值尺度与原场景最终实例尺度可能不同。

**154 个网格的默认材质有缺失纹理引用。** 其中公会实际 prefab 的 36 个底色材质指向缺失的 `CAB-6af7f14628a7155654ed172c566e3677`。已检查全部 8,445 个 Bundle 与 APK 外层 223 个 Unity 数据源，未找到这些精确纹理引用。此类材质显示中性占位色，预览 Actor 名带 `MISSING_TEXTURE`，资产 metadata 保留缺失对象键；不能把它们描述为已恢复原版颜色。其他没有缺失引用的网格仍可能使用特殊 Shader，不等于原效果完全一致。

材质保留源 Shader 名和 JSON 参数，近似映射 BaseColor/纹理、颜色、透明/加法/遮罩混合、顶点色、UV 缩放偏移、基础金属度/粗糙度等。UE FBX 导入翻转 V 后，Unity 的偏移用 `1 - scaleV - offsetV` 转换。特殊描边、扭曲、溶解、渐变、粒子生命周期等不属于完整移植；每个材质的 `DNFM.UnimplementedTextureInputs` 和导入报告逐项记录未实现纹理输入。不能以此宣称 Unity 粒子或角色动画已经成为 UE Niagara/骨骼动画。

有多个实际 prefab 材质变体的网格在来源 manifest 中保留全部绑定，默认优先跨包 MeshRenderer prefab；UE 网格 metadata `DNFM.MaterialVariants` 保存变体材质 ID。可在内容浏览器更换已导入材质。没有来源绑定的网格使用 `M_Unassigned`，不猜造贴图。

网格碰撞使用源表面作为静态查询形状（Complex As Simple）；展示场景中物件关闭碰撞。需要物理模拟的物件要另设简单碰撞。该设置不是 MMO 性能优化或完整碰撞行为重建。

## 重跑流程

在仓库根目录执行（外部源路径默认指向本会话导出，可给 prepare 的 `--source`）：

```powershell
rtk proxy py Scripts/prepare_dnfm_static.py --bootstrap
rtk proxy py Scripts/scan_dnfm_bindings.py
rtk proxy py Scripts/prepare_dnfm_static.py
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMBlender.ps1
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMBlender.ps1 -Verify
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMImport.ps1 -Mode import
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMImport.ps1 -Mode verify
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMImport.ps1 -Mode gallery -Render
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/RunDNFMImport.ps1 -Mode capture -Render
```

UE 脚本只能在编辑器内执行。包装脚本检查已有同工程编辑器，并给每次运行分配独立 run ID，拒绝用旧报告当成功。重建展示图会替换该脚本专属关卡，用户自建场景应另存路径。源码与资源路径一一记录在 `SourceAssets/Unpacked/DNFM130_5_8/manifest.json`。

证据位于 `Artifacts/Tasks/M6-009`：Blender 导出/二进程回读，UE import/verify/gallery/capture 报告，独立渲染截图，跨包材质引用及缺图清单。正式验收状态以任务报告和 TASKS 为准；这份文档只说明接口、用途和限制。
