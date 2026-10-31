# Blender 建模 Agent 执行计划：首职业与一只近战怪物

版本：2026-11-01（Asia/Shanghai）。用途：**仅本机私人学习试玩**。本计划供另一名建模 Agent 逐阶段执行；本次编写计划不代表模型已经制作。目标为 UE-MMO 的第一个多职业研究切片：一个穿完整衣服的格斗家、一个带胸甲和武器的哥布林，之后再制作六招动画/表现与双房间共用件。先完成可验收资源，再启动 UE 功能代码。

## 0. 工作范围和冻结条件

1. 只在 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference 之下建立建模工作区；不修改 D:/DOF/地下城与勇士 原始目录，不直接写 D:/Github-Poj/UE-MMO/Content 的 uasset，不覆盖固定开发包。
2. 已确认：多个独立角色；首职业六技能**包含现有 Z 上挑**。首职业推荐格斗家，待交付的五个额外技能候选为 W 单踢、E 下段踢、R 抛沙、A 地裂、S 踏地。职业和这五个具体名称属于设计候选；先以格斗家造型做美术试样，技能动作批量制作要等造型审阅通过。
3. 已确认：用户只在本机私人学习试玩。所有原 DNF 帧、参考图、建模试样和贴图只保存在本机，不公开、不推送远端、不默认为可发行资源。若用途改变，暂停并重新确认许可。
4. 用户此前认可“橙色上衣＋深色长裤＋运动鞋”的格斗家与“绿皮＋胸甲＋腰部布衣＋木骨武器”的哥布林方向，并进一步要求角色和怪物有更多服装细节。因此首轮无需再询问是否裸体或仅用平面图；但对缺失的正/背面细节要给转台供用户审阅。
5. 原始 2D 帧不构成完整三视图，背面、厚度、腰部布料、胸甲系带为设计推测。禁止宣称“精确还原原作 3D 模型”。像素帧可以指导轮廓、色块、动作节奏，不能直接当 UV、法线或 3D 骨骼数据。

## 1. 环境、资料和尺寸基线

**已验证的 Blender 路径**：

    D:\DOF\blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release\blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release\blender.exe

执行 --version 与后台 Python 表达式均成功；程序报告 Blender 5.2.4 LTS Release Candidate，build hash 4f5026e99892。该版是候选版，不把它写成最终稳定版；Agent 在首次保存及导出 FBX/GLB 后须做重新打开测试。如果出现格式或 Python API 问题，记录故障并在隔离目录复现，不擅自升级或改用户配置。

建模输入：

| 内容 | 精确入口 | 用途 |
|---|---|---|
| 角色服装多姿势叠层 | D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/model-reference/fighter_outfit_alignment.png | 对照橙色上衣、深色裤子、鞋、头发/可选头饰；五个姿势 |
| 怪物胸甲与武器叠层 | D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/model-reference/goblin_armor_weapon_alignment.png | 对照绿皮体型、灰色胸甲和木骨武器 |
| 原始关键帧与元数据 | D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/model-reference/samples 与 manifest.json | 逐帧 PNG、SpriteX/Y、帧尺寸及来源 NPK/IMG；叠层应按公共画布对齐 |
| 所有原始 IMG | D:/DOF/DNF-REA-Output/assets/img | 需要更多关键帧时按原特征名定位；不一次性导入 Blender |
| 源配置、动作 | D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/pvf 与 Docs/DNF-Research/01-技能系统.md（完整 PVF 仍在 D:/DOF/DNF-REA-Output/pvf） | 技能动作节奏和怪物行为参考，不直接把 PVF 数字当 UE 单位 |
| 完整服装规格 | D:/Github-Poj/UE-MMO/Docs/DNF-Research/09-首职业与怪物服装细化设计-待审阅.md | 模型部件及穿模验收基线 |

建模坐标：Blender Z 向上，根骨骼放双脚地面中点，游戏面朝 +X，Y 表示地面纵深。设计以 1 Blender 单位约等于 1 米作为工作约定，导出后在 UE 验证厘米尺寸，不能只凭 Blender 视口看起来正确。现有 UE 角色 Capsule 半径 36 cm、半高 96 cm；近战敌人半径 34 cm、半高 88 cm；模型体型需与这些碰撞外壳协调，但不强行把怪物伸高到角色身高。现有 Manny/Quinn 网格在运行时有 -90° 相对 Yaw 校准；由 UE 导入验证决定新网格是否沿用此校准，不在 Blender 中凭猜测额外转 90°。

### Blender 命令模板

在 PowerShell 中使用 RTK 包裹；先执行：

    rtk proxy powershell.exe -NoProfile -Command '& "D:/DOF/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender.exe" --version'

建模脚本经检查后可用后台模式运行：

    rtk proxy powershell.exe -NoProfile -Command '& "D:/DOF/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release/blender.exe" --background --factory-startup --python-exit-code 2 --python "D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/blender-work/scripts/<阶段脚本>.py"'

其中 <阶段脚本>.py 是后续建模 Agent 应提交的实际脚本名称；当前文件尚不存在。不要仅凭进程退出码认定成功，脚本须输出自己的 JSON 报告，重新打开 .blend 并渲染截图。

## 2. 产物目录与命名约定

新建 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/blender-work：

    D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/blender-work/
      references/              # 本地工作副本与来源 SHA-256；原始提取目录只读
      concepts/                # 缺失角度的设计补全和用户审阅图
      hero_fighter/
        source/                # .blend 分阶段源
        textures/              # PNG/TGA 贴图及 UV 图
        exports/               # hero_fighter_mesh.fbx、每条动作单独 FBX/GLB
        renders/               # 正/侧/背/双 3/4、转台和变形测试帧
        reports/               # 结构、面数、骨骼、动作、贴图与验证 JSON
      enemy_goblin/
        source/ textures/ exports/ renders/ reports/
      scripts/                 # 可重跑生成/校验脚本
      manifest.json            # 每个交付文件的来源、用途、版本和 SHA-256

命名用稳定英文 snake_case。Blender Collection 建议为 REF（参考图，导出时排除）、BODY、CLOTHING、HAIR、ACCESSORY、ARMATURE、WEAPON、CAMERA_LIGHT。对象名例如 hero_body、hero_coat、hero_pants、hero_shoes_l/r、goblin_armor、goblin_waistcloth、goblin_weapon。不得把 2D 参考平面或灯光烘进游戏网格。每阶段新源文件另存；不要覆盖通过审阅的上一阶段 .blend。

## 3. 分阶段模型任务（每阶段都有可审阅交付）

### ART-01：清点参考并冻结造型

1. 从 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/model-reference/manifest.json 与 samples PNG 的元数据读取来源 NPK/IMG、帧索引、SpriteX/Y 和公共画布大小；核验 PNG 可打开。选格斗家第 5 帧作正面偏侧站姿基准、第 13 帧作侧面动作参考、第 31 帧作高踢轮廓参考；哥布林 body0 第 0 帧作基准。原帧并非严格同投影角度，禁止把不同姿势直接当正交三视图。
2. 将角色皮肤、头发、上衣、裤、鞋、可选帽/腰带按原坐标叠合；哥布林身体、胸甲、武器同样叠合。记录每层颜色、覆盖区域和遮挡顺序。检查衣服/头发片是否来自同一版本；不混用不相容装扮。
3. 在 concepts 中画/程序生成三视草案：已知侧视保留源轮廓；前面和背面标记“推测设计”。明确胸甲背带、裤腰、鞋底、头发后部等如何闭合。

**交付**：concept_front.png、concept_side.png、concept_back.png、reference_map.json、来源哈希表。**通过条件**：格斗家上衣、裤、鞋和头发可辨；哥布林胸甲、下装与武器可辨；缺失视角已标注，用户审阅造型方向。此阶段不需要动画。

### ART-02：格斗家穿衣低模与转台

1. 先做身体比例代理：头、胸腹、双臂、双腿、脚各有真实体积与可变形拓扑；Z=0 为脚底。参考 UE 角色 Capsule，侧视肩宽和腿长用第 5 帧校对。
2. 在身体外建立独立衣片：橙色短上衣（肩、胸、领口、背部、边缘）、深色长裤（腰头/膝部/裤脚）、运动鞋（鞋面/厚底/踝口）、手套或护腕、棕色头发。帽与腰带可先做开关部件，默认造型不依赖额外帽子才完整。
3. 每件衣服前后面要闭合，非单面平面；留关节变形余量。身体被衣物完全覆盖的区域不必留下高密度重叠面。颜色参考像素色块，但在 Blender 生成新的 UV 和材质，不把原侧帧硬贴到整个人体。
4. 做侧面、正面、背面、左右 3/4 和连续转台渲染。专门检查腿部和手肘；高踢姿势测试上衣下缘与裤裆穿模。

**交付**：hero_fighter_blockout.blend、6 方向 PNG、转台 MP4 或图序列、blockout_report.json。**通过条件**：服装与头发完整，背面闭合，角色从侧视可读且斜视有体积；只是 2D 平面挤出或无衣裸模均不通过。未审阅前不投入完整雕刻贴图。

### ART-03：哥布林低模与装备转台

1. 按 body0 的短身、前伸头部、长耳和绿色皮肤做有体积网格，比例低于/区别于玩家。灰色胸甲有厚度、背面绑带和肩部固定方式；胸甲下面加粗布内衬/背心。
2. 腰部设计短裤或分片围裙，配腰带、腕带和脚踝裹布/护具；这些是用户要求“有衣服”而原侧图未充分给出的设计补全，转台报告要明确标为推测。
3. 武器独立建模，保留木柄与骨刺/齿形轮廓，手持点按右手骨骼或约定插槽设置；不是命中碰撞的权威来源。双腿、耳朵、武器有合理前后体积。
4. 渲染侧/正/背/3/4 与手握武器的预警姿势，检查胸甲、腕带和下装遮挡。

**交付**：enemy_goblin_blockout.blend、6 方向 PNG、转台、armor_weapon_report.json。**通过条件**：不是裸绿皮；胸甲/衣物/鞋或裹脚/武器从前后 3D 角度都可见；用户确认设计补全。

### ART-04：细化拓扑、UV 与材质

1. 通过 ART-02/03 审阅后，优化头脸、耳朵、衣缝、胸甲系带、鞋底/护腕与武器；服装主体优先轮廓，细纹理不靠超密几何堆砌。角色软组织和关节保留变形环线，胸甲/武器等硬表面分别建模。
2. 为身体、服装、头发、怪物护甲分别展 UV；生成基础颜色、粗糙度和必要的法线/遮罩。自制贴图记录来源和工作文件；从私有参考直接采样颜色只留研究用途。检查背面纹理、UV 重叠和不必要的透明材质。
3. 指导预算：玩家身体与服装合计 15k–30k 三角面、哥布林 8k–18k，主体贴图优先 2K、配件 1K；这些是**性能目标，不是硬上限或原资源事实**。超限须报告原因并在 UE 性能试验后调整。至少做一套可简化的 LOD 方案。

**交付**：hero_fighter_detail.blend、enemy_goblin_detail.blend、各贴图及 UV 图、turntable_final.png/视频、poly_material_report.json。**通过条件**：无明显裸露缺件、破面、翻法线或多余遮挡重叠；高踢/挥拳/转身仍可辨服装结构。

### ART-05：骨骼、蒙皮与动作

1. 玩家优先选择可与 UE Manny 动画重定向的标准人形骨架；如果采用自建骨骼，交 bone_map.json，列 root/pelvis/spine/chest/neck/head/肩/肘/手/髋/膝/踝、左右及武器握点。哥布林可独立骨架，但须交相同结构清单。Root 在脚底，不把相机或参考板绑定进骨架。
2. 服装与身体共享动作，分部件检查蒙皮。必测四组极端姿势：双手护面/出拳、抬腿高踢、上挑、蹲伏/受击；每个姿势都拍正面/侧面，记录肩、腋、胯、膝、鞋、胸甲、腰布穿模情况及修复。
3. 初始动作交付：玩家 idle、X 跑、Y 前后走/转向、jump_start/fall/land、两段普攻、launcher、aerial、singlekick、damagelowkick、throwsand、earthbreak、stomp、hit、knockdown、getup、death。哥布林 idle、move、telegraph、attack、hit、death，必要时加 down/getup。技能如含起手/有效/收招三个阶段，用一个 Action 的可验证标记或分 Action，并明确每阶段秒数；不能把 2D 帧索引误当 UE 60 Hz 时间。
4. 每条动画交 Action ID、帧率、时长、是否循环、Root Motion、命中关键帧、结束姿态、可否转向；导出独立动作文件及动画清单。不够完整的动作要标 placeholder，不得拿“同一个挥拳”重命名充六招。

**交付**：hero_fighter_rig.blend、enemy_goblin_rig.blend、actions_manifest.json、逐动作预览与衣物变形报告。**通过条件**：动作可在多角度观察，脚不明显漂浮，所有模型部件跟骨骼运动，五招加 Z 上挑均可辨认，敌人武器抓握稳定。

### ART-06：导出与独立复核

1. 将角色/怪物静态外观、骨骼、每条动作单独导出 FBX 或按 UE 测试过的可兼容格式；贴图保留相对路径，不包含参考平面、灯、相机。保存原 .blend，不只交导出文件。
2. 在 Blender 新进程重新打开最终 .blend，检查对象/骨骼/动作/贴图路径及文件数量。建模 Agent 到这里交出可导入包，不直接写 UE-MMO 工程。之后由 UE 资源 Agent 领取新的资源任务卡，在 UE 编辑器的独立命名空间导入验证比例、轴向、骨骼、材质、动画帧与资源引用，保存后第二个 UE 进程重载。此步骤属于**资源验收**，不接入正式战斗代码、菜单或房间流程。
3. 最终对照 UE 固定侧视相机做侧视、斜前和斜后截图，检查玩家站地、怪物站地、胸甲/裤/鞋无遮挡及技能动作命中姿态。导入有误先回 Blender 修正，不在 UE 中用不可追溯临时比例凑合。

**交付**：建模 Agent 交导出文件、最终 .blend、textures、manifest.json、blender_validation.json；UE 资源 Agent 接着交 ue_import_validation.json、复载结果和截图路径。**通过条件**：文件可二次导入、骨骼/贴图不丢、尺寸与碰撞可协调、无关键报错；两段资源验收均通过后才可交给 UE 功能 Agent。

## 4. 最终交接验收表

建模 Agent 必须按资源分别填写 OK / BLOCKED / PLACEHOLDER，而不是只写“完成模型”：

| 验收项 | 格斗家 | 哥布林 |
|---|---|---|
| 侧/正/背/两侧 3/4 与转台渲染 | 必须 | 必须 |
| 有体积网格与闭合衣物/护具 | 上衣、裤、鞋、头发、手部护具 | 胸甲、内衬、腰部衣物、手脚护具、武器 |
| 统一材质/UV/贴图无丢失 | 必须 | 必须 |
| 骨骼、蒙皮、极端动作无明显穿模 | 必须 | 必须 |
| 完整动作清单与命中关键帧 | 两普攻、上挑、空追、五热栏技能、受击等 | 移动、预警、攻击、受击、死亡 |
| .blend 源＋FBX/GLB＋纹理＋来源哈希 | 必须 | 必须 |
| UE 临时导入、保存、重启重载、渲染检查 | 必须 | 必须 |

任何阶段出现“缺一条技能动作、背面只有空白贴片、衣服未蒙皮、怪物胸甲漂浮”等问题，明确 BLOCKED 或 PLACEHOLDER 并给下一步；不能把制作进度写入 UE-MMO 旧任务 DONE。既有 UE 工程本轮无人对其建模任务改状态；若未来开 UE 资源任务，先创建新任务卡并按 TASKS/HANDOFF 规则登记。

## 5. 可直接发送给另一个建模 Agent 的任务说明

> 请在本机 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference 下完成一个私人学习用 Blender 5.2.4 LTS RC 建模任务。先阅读 Docs/DNF-Research/10-Blender建模Agent执行计划.md、Docs/DNF-Research/09-首职业与怪物服装细化设计-待审阅.md、Docs/DNF-Research/08-Blender首轮参考候选-待确认.md，核对 SourceAssets/Unpacked/DNFPrivateReference/model-reference/manifest.json 与已合成 PNG；不要改 D:/DOF/地下城与勇士 原件、D:/Github-Poj/UE-MMO 代码/uasset、TASKS.md 或固定开发包。使用文档中的 blender.exe 路径。第一轮只做 ART-01 参考与 ART-02/03 穿衣格斗家和有胸甲、腰部衣物、武器的哥布林低模转台，给正/侧/背及两个 3/4 的可视截图、.blend 源、来源哈希与问题列表，不先做六技能或 UE 功能代码。用户审阅第一轮转台后，再逐阶段完成细化、骨骼、动作和导出。侧帧缺失的 3D 结构标为设计推测；不公开或推送提取资源。每个阶段做实际文件与画面核验，未通过不得标完成。
