"""Read-only validation of migrated DNF design docs and private references."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
docs = root / "Docs" / "DNF-Research"
private = root / "SourceAssets" / "Unpacked" / "DNFPrivateReference"
manifest = json.loads((private / "migration-manifest.json").read_text(encoding="utf-8"))
issues: list[str] = []

expected_docs = {
    "README.md",
    "01-逆向研究依据.md",
    "02-多职业功能实施方案.md",
    "03-Blender建模与资源交接.md",
}
actual_docs = {p.name for p in docs.glob("*.md")}
if actual_docs != expected_docs:
    issues.append(f"unexpected final document set: {sorted(actual_docs ^ expected_docs)}")
source_docs = [p for p in docs.glob("*.md") if p.name != "README.md"]

for document in [*source_docs, docs / "README.md"]:
    body = document.read_text(encoding="utf-8")
    for relative in re.findall(r"\]\(([^)]+)\)", body):
        if relative.startswith(("https://", "http://", "#")):
            continue
        if not (document.parent / relative).resolve().exists():
            issues.append(f"broken link: {document.name}: {relative}")

records = manifest.get("files", [])
private_records = [row for row in records if row["destination"].startswith("SourceAssets/")]
if len(private_records) != 317:
    issues.append(f"expected 317 private source records, found {len(private_records)}")
for record in private_records:
    path = root / record["destination"]
    if not path.is_file():
        issues.append(f"missing: {record['destination']}")
        continue
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    if sha != record["sha256"]:
        issues.append(f"hash mismatch: {record['destination']}")

samples = list((private / "model-reference" / "samples").glob("*.png"))
if len(samples) != 218:
    issues.append(f"expected 218 PNG samples, found {len(samples)}")
raw_img = list((private / "img").rglob("*.img"))
if len(raw_img) != 10:
    issues.append(f"expected 10 raw IMG examples, found {len(raw_img)}")

result = {
    "success": not issues,
    "documents": len(source_docs),
    "private_records": len(private_records),
    "sample_pngs": len(samples),
    "raw_img_examples": len(raw_img),
    "issues": issues,
}
print(json.dumps(result, ensure_ascii=False))
raise SystemExit(0 if not issues else 1)
