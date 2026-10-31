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

source_docs = [p for p in docs.glob("*.md") if p.name != "README.md"]
if len(source_docs) != 11:
    issues.append(f"expected 11 authored documents, found {len(source_docs)}")

for document in [*source_docs, docs / "README.md"]:
    body = document.read_text(encoding="utf-8")
    for relative in re.findall(r"\]\(([^)]+)\)", body):
        if relative.startswith(("https://", "http://", "#")):
            continue
        if not (document.parent / relative).resolve().exists():
            issues.append(f"broken link: {document.name}: {relative}")

records = manifest.get("files", [])
if len(records) != 328:
    issues.append(f"expected 328 copied source files, found {len(records)}")
for record in records:
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
    "copied_records": len(records),
    "sample_pngs": len(samples),
    "raw_img_examples": len(raw_img),
    "issues": issues,
}
print(json.dumps(result, ensure_ascii=False))
raise SystemExit(0 if not issues else 1)
