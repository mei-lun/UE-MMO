# Blender 首轮 2D 参考候选

日期：2026-11-01。用户已确认用途仅本机私人学习试玩。本页素材、PNG 样本和后续模型参考均限定本机研究，不认定获得公开发布权。已经完成四套图片接触表，尚未开始 Blender 建模。

| 用途 | NPK 包 | IMG 路径示例 | 检查重点 |
|---|---|---|---|
| 格斗家身体与姿势 | sprite_character_fighter_equipment_avatar_skin.NPK | sprite/character/fighter/equipment/avatar/skin/ft_body0000.img | 先挑完整身体与静止/移动帧，不用缩略图代替姿势 |
| 格斗家头发 | sprite_character_fighter_equipment_avatar_hair.NPK | sprite/character/fighter/equipment/avatar/hair/ft_hair0000a.img | 与身体是否同一职业造型、前后遮挡是否正确 |
| 格斗家上装 | sprite_character_fighter_equipment_avatar_coat.NPK | sprite/character/fighter/equipment/avatar/coat/ft_coat0000a.img | 已避开 (tn) 缩略版本；该层像素非常小，必须按原偏移与身体叠合后审阅 |
| 哥布林 | sprite_monster_goblin.NPK | sprite/monster/goblin/body0.img | 造型、动作帧、是否包含多方向；缺失角度须在建模时另行决定 |
| 技能视觉 | sprite_character_fighter_effect.NPK | sprite/character/fighter/effect/skillicon.img 及其他 effect IMG | 图标、起手、命中特效必须分别归类；不要把特效帧当人物动作 |

完整清单：../../SourceAssets/Unpacked/DNFPrivateReference/reports/npk-inventory.json；已拆出的 IMG：D:/DOF/DNF-REA-Output/assets/img；已解的 PNG 样本：D:/DOF/DNF-REA-Output/assets/images。人体装备的衣服、皮肤、头发是分层帧，必须先挑一套同版本组合并合成多张参考图。当前 fighter skin 包 22 个 IMG、3,542 帧，hair 包 1,330 个 IMG、214,610 帧，coat 包 1,869 个 IMG、301,263 帧；全量堆入 Blender 会使参考失焦，应先缩小到一套外观和少量关键姿势。哥布林包 19 个 IMG、336 帧。

已生成本机 PNG 样本：../../SourceAssets/Unpacked/DNFPrivateReference/model-reference/samples（人体/头发/上装各 24 帧，哥布林 17 帧），对应 manifest.json；同目录的 fighter_body_sheet.png、fighter_hair_sheet.png、fighter_coat_sheet.png 和 goblin_body_sheet.png 可用于先选造型。实看发现身体图包含拳脚与跑动姿势，头发/上装是定位碎片而不是完整立绘；需要依 IMG 帧元数据和动画指针对齐，不能简单铺到身体像素上。

首个放大对照图已保存为 ../../SourceAssets/Unpacked/DNFPrivateReference/model-reference/first_model_review.png：fighter 使用 ft_body0000.img 的第 5 帧，哥布林使用 body0.img 的第 0 帧。它帮助审阅比例、轮廓和外观方向，不是 Blender 3D 成果，也没有解决衣服/头发的帧偏移。已确认当前只用于本机私人学习试玩；不推送本图或衍生资源。

首轮可视建模目标仍是职业角色站姿正交侧视、前/后 3/4 推测视角及旋转展示；试样经用户审阅后再扩展六技能和敌人。单侧图片无法确定背面，审阅图应对推测区域做标注。开发包资产仅在研究范围、资源来源及导入门槛确认后处理。
