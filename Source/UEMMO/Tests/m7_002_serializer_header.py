# -*- coding: utf-8 -*-
"""Re-apply the serializer HEADER edits for M7-002 (run from repo root)."""

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


apply('Source/UEMMO/Persistence/ProfileSerializer.h', [
    ('''	/** M6-003: the same RoomId appears in more than one RoomClears row. */
	DuplicateRoomClearRecord
};''',
'''	/** M6-003: the same RoomId appears in more than one RoomClears row. */
	DuplicateRoomClearRecord,

	/**
	 * M7-002: a schema_version 4 row fails its profession-state field rules
	 * (an empty/non-id ProfessionId, an empty skill id, a rank outside
	 * 1..8, UnspentSP or DungeonClears negative, a loadout slot outside
	 * 1..8). Rejected, never clamped.
	 */
	InvalidProfessionState,

	/**
	 * M7-002: the same skill id appears in more than one SkillRanks row, or
	 * the same loadout slot appears in more than one SkillLoadout row - set
	 * semantics must be restored, never silently deduplicated.
	 */
	DuplicateProfessionState,

	/**
	 * M7-002: a SkillLoadout row references a skill without a learned rank
	 * row (a dangling hotbar assignment can never launch anything).
	 */
	DanglingLoadoutSkill
};'''),
    ('''/**
 * M3-013 (extended by M5-046): pure, World-free profile save serializer''',
'''/**
 * M7-002: the profession-state inputs of ToSaveGame. The defaults are the
 * legacy-equivalent shape (ProfessionId "prototype", the four legacy attacks
 * light_01/light_02/launcher/aerial_01 at rank 1, UnspentSP 0, an empty
 * loadout, no revision, DungeonClears 0), so every pre-existing call site
 * compiles and writes a legal v4 payload unchanged.
 */
struct FProfessionSaveState
{
	/** The character's profession id (a Data/Professions profession_id). */
	FName ProfessionId = FName(TEXT("prototype"));

	/** The learned skill ranks (skill id -> rank 1..max_rank). */
	TArray<FSkillRankSaveEntry> SkillRanks;

	/** Unspent skill points (>= 0). */
	int32 UnspentSP = 0;

	/** The recorded default hotbar assignments (slot 1..8 -> skill). */
	TArray<FSkillSlotSaveEntry> SkillLoadout;

	/** The profession-config provenance (empty when unknown). */
	FString ProfessionConfigRevision;

	/** Completed continuous dungeon runs (>= 0). */
	int32 DungeonClears = 0;

	/** The legacy-equivalent default shape (see the struct comment). */
	static FProfessionSaveState MakeLegacyPrototype();

	/** The frozen legacy attack ids the legacy shape grants at rank 1. */
	static const TArray<FName>& LegacyPrototypeSkillIds();
};

/**
 * M7-002: the optional profession-state outputs of FromSaveGame. Every field
 * is exactly what a v4 save carried; a v1/v2/v3 input returns the MIGRATED
 * state (ProfessionId "prototype", the four legacy attacks at rank 1,
 * UnspentSP 0, an empty loadout, no revision, DungeonClears 0). Never written
 * on a failed load.
 */
struct FProfessionStateOut
{
	/** The character's profession id (e.g. "prototype" for a migrated legacy save). */
	FName ProfessionId;

	/** The learned skill ranks (a migrated legacy save: the four legacy attacks at rank 1). */
	TArray<FSkillRankSaveEntry> SkillRanks;

	/** Unspent skill points (a migrated legacy save: 0). */
	int32 UnspentSP = 0;

	/** The recorded default hotbar assignments (a migrated legacy save: empty). */
	TArray<FSkillSlotSaveEntry> SkillLoadout;

	/** The profession-config provenance (empty when unknown). */
	FString ProfessionConfigRevision;

	/** Completed continuous dungeon runs (a migrated legacy save: 0). */
	int32 DungeonClears = 0;
};

/**
 * M3-013 (extended by M5-046): pure, World-free profile save serializer'''),
    ('''		bool bInWeaponStateInitialized = false,
		const FString& InConfigRevision = FString(),
		const TArray<FRoomClearSaveEntry>& InRoomClears = TArray<FRoomClearSaveEntry>(),
		UObject* Outer = nullptr);''',
'''		bool bInWeaponStateInitialized = false,
		const FString& InConfigRevision = FString(),
		const TArray<FRoomClearSaveEntry>& InRoomClears = TArray<FRoomClearSaveEntry>(),
		const FProfessionSaveState* InProfessionState = nullptr,
		UObject* Outer = nullptr);'''),
    ('''		TArray<FRoomClearSaveEntry>* OutRoomClears = nullptr);''',
'''		TArray<FRoomClearSaveEntry>* OutRoomClears = nullptr,
		struct FProfessionStateOut* OutProfessionState = nullptr);'''),
    ('''	static UProfileSaveGame* MigrateV2ToV3(const UProfileSaveGame& LegacySave,
		FProfileLoadError& OutError,
		UObject* Outer = nullptr);''',
'''	static UProfileSaveGame* MigrateV2ToV3(const UProfileSaveGame& LegacySave,
		FProfileLoadError& OutError,
		UObject* Outer = nullptr);

	/**
	 * M7-002: the explicit one-time v3 -> v4 migration (the third migration
	 * channel). Refuses anything but a valid schema_version 3 input; any other
	 * version is a caller bug. v4 adds ONLY the profession state, so the
	 * migration copies every v3 field verbatim and seeds the new columns with
	 * the legacy-equivalent shape: ProfessionId "prototype", SkillRanks =
	 * the four legacy attacks (light_01/light_02/launcher/aerial_01) at
	 * rank 1 (a legacy player is never locked out of the normal attacks, and
	 * the legacy attacks do not consume MP or SP), UnspentSP 0, an EMPTY
	 * loadout (the legacy profession has no hotbar skills), an empty
	 * ProfessionConfigRevision and DungeonClears 0. Nothing stays implicit;
	 * the original save stays byte-identical either way. Returns null on any
	 * refusal (OutError names the reason).
	 */
	static UProfileSaveGame* MigrateV3ToV4(const UProfileSaveGame& LegacySave,
		FProfileLoadError& OutError,
		UObject* Outer = nullptr);'''),
])
print('DONE')
