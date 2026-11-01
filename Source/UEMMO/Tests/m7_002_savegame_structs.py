# -*- coding: utf-8 -*-
"""Append the ProfileSaveGame struct + v4 field edits (run from repo root)."""

def apply(path, edits):
    from pathlib import Path
    p = Path(path)
    s = p.read_text(encoding='utf-8')
    n = 0
    for old, new in edits:
        if new in s:
            continue
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
        n += 1
    p.write_text(s, encoding='utf-8')
    print('applied', path, n, 'new edits')


STRUCTS = '''/**
 * Serialized form of one per-skill learned rank (schema_version=4, M7-002).
 * Pure value row: the skill id (a Data/Professions/skills.json skill_id) and
 * the rank the character has purchased/been granted (1..max_rank).
 */
USTRUCT()
struct FSkillRankSaveEntry
{
	GENERATED_BODY()

	/** Business identity of the skill (e.g. "launcher_z"). */
	UPROPERTY()
	FName SkillId;

	/** The character's owned rank of this skill (1..max_rank; >= 1 in any valid row). */
	UPROPERTY()
	int32 Rank = 0;
};

/**
 * Serialized form of one default hotbar assignment (schema_version=4, M7-002):
 * the slot index (1..8, Q/W/E/R/A/S/D/F) and the skill it launches. A skill
 * without a learned rank row is dangling and rejected on load.
 */
USTRUCT()
struct FSkillSlotSaveEntry
{
	GENERATED_BODY()

	/** Hotbar slot index (1..8; Q=1, W=2, E=3, R=4, A=5, S=6, D=7, F=8). */
	UPROPERTY()
	int32 SlotIndex = 0;

	/** The skill this slot launches (must have a learned rank row). */
	UPROPERTY()
	FName SkillId;
};

/**
 * M3-013: the versioned profile save structure (interface contract section 8).'''

apply('Source/UEMMO/Persistence/ProfileSaveGame.h', [
    ('''/**
 * M3-013: the versioned profile save structure (interface contract section 8).''', STRUCTS),
])
print('DONE')
