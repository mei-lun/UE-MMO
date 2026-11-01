# -*- coding: utf-8 -*-
"""Re-apply the serializer CPP ToSaveGame edits for M7-002 (run from repo root)."""

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


apply('Source/UEMMO/Persistence/ProfileSerializer.cpp', [
    ('''	const TArray<FRoomClearSaveEntry>& InRoomClears,
	UObject* Outer)
{
	UProfileSaveGame* Save = NewObject<UProfileSaveGame>(Outer ? Outer : GetTransientPackage());

	Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
	Save->CharacterId = Snapshot.CharacterId;
	Save->Level = Snapshot.Level;
	Save->XP = Snapshot.XP;
	// Settings placeholder: no settings domain exists yet, so the serializer
	// always writes the documented default (reported as a known limitation).
	Save->MasterVolume = 1.0f;''',
'''	const TArray<FRoomClearSaveEntry>& InRoomClears,
	const FProfessionSaveState* InProfessionState,
	UObject* Outer)
{
	UProfileSaveGame* Save = NewObject<UProfileSaveGame>(Outer ? Outer : GetTransientPackage());

	Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
	Save->CharacterId = Snapshot.CharacterId;
	Save->Level = Snapshot.Level;
	Save->XP = Snapshot.XP;
	// Settings placeholder: no settings domain exists yet, so the serializer
	// always writes the documented default (reported as a known limitation).
	Save->MasterVolume = 1.0f;

	// M7-002: the profession state. nullptr stamps the legacy-equivalent
	// default shape (prototype + the four legacy attacks at rank 1), so every
	// pre-existing call site writes a legal v4 payload unchanged.
	const FProfessionSaveState ResolvedProfession = InProfessionState
		? *InProfessionState
		: FProfessionSaveState::MakeLegacyPrototype();
	Save->ProfessionId = ResolvedProfession.ProfessionId;
	Save->SkillRanks = ResolvedProfession.SkillRanks;
	Save->UnspentSP = ResolvedProfession.UnspentSP;
	Save->SkillLoadout = ResolvedProfession.SkillLoadout;
	Save->ProfessionConfigRevision = ResolvedProfession.ProfessionConfigRevision;
	Save->DungeonClears = ResolvedProfession.DungeonClears;'''),
    ('''UProfileSaveGame* FProfileSerializer::ToSaveGame(const FProfileSnapshot& Snapshot,''',
'''// M7-002: the frozen legacy attack ids. The four normal attacks of the
// research prototype (Data/combat-attacks.json) - a migrated legacy save
// carries exactly these at rank 1 so a legacy player is never locked out of
// the normal attacks. Frozen content: the list is a checked-in contract, the
// same ids the M1-009 tests pin.
static const TArray<FName>& GetLegacyPrototypeAttackIds()
{
	static const TArray<FName> Ids = {
		FName(TEXT("light_01")),
		FName(TEXT("light_02")),
		FName(TEXT("launcher")),
		FName(TEXT("aerial_01"))
	};
	return Ids;
}

FProfessionSaveState FProfessionSaveState::MakeLegacyPrototype()
{
	FProfessionSaveState State;
	State.ProfessionId = FName(TEXT("prototype"));
	for (const FName& Attack : GetLegacyPrototypeAttackIds())
	{
		FSkillRankSaveEntry RankRow;
		RankRow.SkillId = Attack;
		RankRow.Rank = 1;
		State.SkillRanks.Add(RankRow);
	}
	State.UnspentSP = 0;
	State.SkillLoadout.Reset();
	State.ProfessionConfigRevision.Reset();
	State.DungeonClears = 0;
	return State;
}

const TArray<FName>& FProfessionSaveState::LegacyPrototypeSkillIds()
{
	return GetLegacyPrototypeAttackIds();
}

UProfileSaveGame* FProfileSerializer::ToSaveGame(const FProfileSnapshot& Snapshot,'''),
])
print('DONE')
