"""Copy bounded DNF study documents and local-only references into UE-MMO.

This script does not move or delete the original archive. References are placed
under an existing ignored directory; only Markdown is intended for Git.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
SOURCE = Path("D:/DOF/DNF-REA-Output")
DOCS = PROJECT / "Docs" / "DNF-Research"
PRIVATE = PROJECT / "SourceAssets" / "Unpacked" / "DNFPrivateReference"


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


rows: list[dict[str, object]] = []


def copy_file(src: Path, dst: Path) -> None:
    if not src.is_file():
        raise FileNotFoundError(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    source_digest = digest(src)
    target_digest = digest(dst)
    if source_digest != target_digest:
        raise RuntimeError(f"SHA-256 mismatch: {src} -> {dst}")
    rows.append({
        "source": str(src),
        "destination": str(dst.relative_to(PROJECT)).replace("\\", "/"),
        "bytes": dst.stat().st_size,
        "sha256": target_digest,
    })


DOCS.mkdir(parents=True, exist_ok=True)
PRIVATE.mkdir(parents=True, exist_ok=True)

# Keep the eleven authored research/design documents together. The original
# numbering is preserved to make earlier chat references easy to trace.
for src in sorted((SOURCE / "docs").glob("*.md")):
    copy_file(src, DOCS / src.name)

root_refs = [
    "manifest.json",
    "fighter_body_sheet.png",
    "fighter_hair_sheet.png",
    "fighter_coat_sheet.png",
    "goblin_body_sheet.png",
    "fighter_outfit_alignment.png",
    "goblin_armor_weapon_alignment.png",
    "goblin_equipped_alignment.png",
    "first_model_review.png",
    "compose_outfit_preview.py",
    "make_contact_sheets.py",
    "make_review_preview.py",
]
for name in sorted(set(root_refs)):
    copy_file(SOURCE / "model-reference" / name, PRIVATE / "model-reference" / name)

for src in sorted((SOURCE / "model-reference" / "samples").glob("*.png")):
    copy_file(src, PRIVATE / "model-reference" / "samples" / src.name)

reports = [
    "rea-evidence.json",
    "pvf-summary.json",
    "img-summary.json",
    "raw-assets-summary.json",
    "sound-npk-summary.json",
    "video-summary.json",
    "system-counts.json",
    "verification.json",
    "skills.csv",
    "skill-links.csv",
    "professions.csv",
    "dungeons.csv",
    "maps.csv",
    "squirrel-scripts.csv",
    "image-packages.csv",
    "resource-references.csv",
    "npk-inventory.json",
]
for name in reports:
    copy_file(SOURCE / "reports" / name, PRIVATE / "reports" / name)

pvf_refs = [
    "character/fighter/fighter.chr.json",
    "monster/goblin/goblin.mob.json",
    "dungeon/act1/lorien.dgn.json",
    "map/lorien/s0.map.json",
    "map/lorien/b1.map.json",
]
pvf_refs += [f"skill/fighter/{name}.skl.json" for name in (
    "liftupper", "singlekick", "damagelowkick", "throwsand", "earthbreak", "stomp"
)]
for name in pvf_refs:
    copy_file(SOURCE / "pvf" / name, PRIVATE / "pvf" / name)

# Preserve the original small image-decoder source as a local research tool,
# excluding the nested cloned .git and installed node_modules distributions.
for src in sorted((SOURCE / "tools" / "dfo-npk-extractor-bun" / "src").rglob("*.ts")):
    rel = src.relative_to(SOURCE / "tools" / "dfo-npk-extractor-bun")
    copy_file(src, PRIVATE / "tools" / "dfo-npk-extractor-bun" / rel)
for name in ("package.json", "README.md", "LICENSE", "index.ts",
             "extract-model-reference-samples.ts"):
    copy_file(SOURCE / "tools" / "dfo-npk-extractor-bun" / name,
              PRIVATE / "tools" / "dfo-npk-extractor-bun" / name)

# Copy a small indexed subset of raw IMG containers needed to reproduce the
# private fighter/goblin previews, not the complete multi-gigabyte archive.
manifest = json.loads((SOURCE / "model-reference" / "manifest.json").read_text(encoding="utf-8"))
source_by_img = {str(row["img"]): str(row["npk"]) for row in manifest if "img" in row and "npk" in row}
for img, package in sorted(source_by_img.items()):
    package_dir = "monsters" if package.startswith("sprite_monster_") else "characters-and-equipment"
    src = SOURCE / "assets" / "img" / package_dir / package / Path(img)
    dst = PRIVATE / "img" / package / Path(img)
    copy_file(src, dst)

# The authored prose still records the historical external archive. Resolve
# references that are actually staged locally, and route future Blender work
# to the ignored project-local reference area.
private_rel = "../../SourceAssets/Unpacked/DNFPrivateReference"
document_rewrites: list[dict[str, object]] = []
for document in sorted(DOCS.glob("*.md")):
    content = document.read_text(encoding="utf-8")
    content = content.replace("../model-reference/", private_rel + "/model-reference/")
    content = content.replace("../reports/", private_rel + "/reports/")
    content = content.replace("../tools/", private_rel + "/tools/")
    content = content.replace("../assets/img", "D:/DOF/DNF-REA-Output/assets/img")
    content = content.replace("../assets/images", "D:/DOF/DNF-REA-Output/assets/images")
    content = content.replace(private_rel + "/reports/pvf-manifest.json",
                              "D:/DOF/DNF-REA-Output/reports/pvf-manifest.json")
    content = content.replace(private_rel + "/reports/raw-assets-manifest.jsonl",
                              "D:/DOF/DNF-REA-Output/reports/raw-assets-manifest.jsonl")
    content = content.replace(private_rel + "/reports/img-files.jsonl",
                              "D:/DOF/DNF-REA-Output/reports/img-files.jsonl")
    content = content.replace("D:/DOF/DNF-REA-Output/model-reference",
                              "D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/model-reference")
    content = content.replace("D:/DOF/DNF-REA-Output/blender-work",
                              "D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/blender-work")
    content = content.replace("D:/DOF/DNF-REA-Output/docs/",
                              "D:/Github-Poj/UE-MMO/Docs/DNF-Research/")
    if document.name.startswith("10-"):
        content = content.replace("D:/DOF/DNF-REA-Output/pvf 与 docs/01-技能系统.md",
                                  "D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference/pvf 与 Docs/DNF-Research/01-技能系统.md（完整 PVF 仍在 D:/DOF/DNF-REA-Output/pvf）")
        content = content.replace("只在 D:/DOF/DNF-REA-Output 之下建立建模工作区",
                                  "只在 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference 之下建立建模工作区")
        content = content.replace("请在本机 D:/DOF/DNF-REA-Output 下完成",
                                  "请在本机 D:/Github-Poj/UE-MMO/SourceAssets/Unpacked/DNFPrivateReference 下完成")
        content = content.replace("docs/10-Blender建模Agent执行计划.md",
                                  "Docs/DNF-Research/10-Blender建模Agent执行计划.md")
        content = content.replace("docs/09-首职业与怪物服装细化设计-待审阅.md",
                                  "Docs/DNF-Research/09-首职业与怪物服装细化设计-待审阅.md")
        content = content.replace("docs/08-Blender首轮参考候选-待确认.md",
                                  "Docs/DNF-Research/08-Blender首轮参考候选-待确认.md")
        content = content.replace("核对 model-reference/manifest.json 与已合成 PNG",
                                  "核对 SourceAssets/Unpacked/DNFPrivateReference/model-reference/manifest.json 与已合成 PNG")
    document.write_text(content, encoding="utf-8")
    document_rewrites.append({
        "document": str(document.relative_to(PROJECT)).replace("\\", "/"),
        "sha256_after_rewrite": digest(document),
    })

index = DOCS / "README.md"
index.write_text("""# DNF 逆向研究与 UE-MMO 扩展规划

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
""", encoding="utf-8")

# Replace the initial byte-copy receipt with final identities of rewritten
# documents. Private reference copies remain byte-identical to their sources.
for row in rows:
    if str(row["destination"]).startswith("Docs/"):
        row["sha256"] = digest(PROJECT / str(row["destination"]))
        row["bytes"] = (PROJECT / str(row["destination"])).stat().st_size
        row["post_copy_document_rewrite"] = True

manifest_target = PRIVATE / "migration-manifest.json"
manifest_target.write_text(json.dumps({
    "source_root": str(SOURCE),
    "project_root": str(PROJECT),
    "private_use_only": True,
    "file_count": len(rows),
    "total_bytes": sum(int(row["bytes"]) for row in rows),
    "files": rows,
    "document_rewrites": document_rewrites,
}, ensure_ascii=False, indent=2), encoding="utf-8")

print(json.dumps({
    "document_count": len(list(DOCS.glob("*.md"))),
    "copied_files": len(rows),
    "copied_bytes": sum(int(row["bytes"]) for row in rows),
    "private_root": str(PRIVATE),
}, ensure_ascii=False))
