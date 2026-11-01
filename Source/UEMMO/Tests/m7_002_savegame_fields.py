# -*- coding: utf-8 -*-
"""Re-apply the ProfileSaveGame version constants + v4 fields (run from repo root)."""

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


apply('Source/UEMMO/Persistence/ProfileSaveGame.h', [
    ('''	static constexpr int32 CurrentSchemaVersion = 3;

	/** The pre-M5-046 legacy schema version; the migration channel's input. */
	static constexpr int32 LegacySchemaVersion = 1;

	/** The M5-046 pre-M6-003 layout; loads through the v2->v3 migration. */
	static constexpr int32 SchemaVersion2 = 2;''',
'''	static constexpr int32 CurrentSchemaVersion = 4;

	/** The pre-M5-046 legacy schema version; the migration channel's input. */
	static constexpr int32 LegacySchemaVersion = 1;

	/** The M5-046 pre-M6-003 layout; loads through the v2->v3 migration. */
	static constexpr int32 SchemaVersion2 = 2;

	/** The M6-003 pre-M7-002 layout; loads through the v3->v4 migration. */
	static constexpr int32 SchemaVersion3 = 3;'''),
    ('''	UPROPERTY()
	TArray<FRoomClearSaveEntry> RoomClears;
};''',
'''	UPROPERTY()
	TArray<FRoomClearSaveEntry> RoomClears;

	// -- schema_version=4 additions (M7-002) --------------------------------------

	/**
	 * The character's profession (a Data/Professions/professions.json
	 * profession_id). The DEFAULT is "prototype" (the legacy shape), so a
	 * hand-assembled v4 payload without profession inputs is a legal legacy
	 * character. Structural id rules are validated on load; the profession's
	 * EXISTENCE against the live catalog is the consuming layer's gate
	 * (M7-003).
	 */
	UPROPERTY()
	FName ProfessionId = FName(TEXT("prototype"));

	/**
	 * The learned skill ranks (see FSkillRankSaveEntry). A migrated legacy
	 * save carries exactly the four legacy attacks at rank 1 (the M7-002
	 * migration contract: a legacy player is never locked out of the normal
	 * attacks, and the legacy attacks do not consume MP or SP).
	 */
	UPROPERTY()
	TArray<FSkillRankSaveEntry> SkillRanks;

	/** Unspent skill points (>= 0; a migrated legacy save starts at 0). */
	UPROPERTY()
	int32 UnspentSP = 0;

	/** The recorded default hotbar assignments (see FSkillSlotSaveEntry). */
	UPROPERTY()
	TArray<FSkillSlotSaveEntry> SkillLoadout;

	/**
	 * Provenance of the profession config this state was last saved under;
	 * empty when unknown (e.g. right after a migration). Provenance only -
	 * never a load gate by itself (the M5-046 revision rule).
	 */
	UPROPERTY()
	FString ProfessionConfigRevision;

	/**
	 * How many CONTINUOUS dungeon runs finished completely (M7-004's
	 * independent value counter, distinct from the per-room RoomClears
	 * records). A migrated legacy save starts at 0.
	 */
	UPROPERTY()
	int32 DungeonClears = 0;
};'''),
])
print('DONE')
