# -*- coding: utf-8 -*-
"""One-shot applier for the M7-002 persistence/profile edits (run from repo root)."""

def apply(path, edits):
    from pathlib import Path
    p = Path(path)
    s = p.read_text(encoding='utf-8')
    n = 0
    for old, new in edits:
        if new in s:
            continue  # already applied (idempotent rerun)
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
])

apply('Source/UEMMO/Persistence/ProfileSerializer.cpp', [
    ('''	if (Save && Save->SchemaVersion == UProfileSaveGame::SchemaVersion2)
	{
		MigratedScratch = MigrateV2ToV3(*Save, OutError);
		if (!MigratedScratch)
		{
			return false;
		}
		Save = MigratedScratch;
	}''',
'''	if (Save && Save->SchemaVersion == UProfileSaveGame::SchemaVersion2)
	{
		MigratedScratch = MigrateV2ToV3(*Save, OutError);
		if (!MigratedScratch)
		{
			return false;
		}
		Save = MigratedScratch;
	}

	// M7-002: a schema_version 3 save enters through the third migration
	// channel - the profession state seeds the legacy-equivalent shape (see
	// MigrateV3ToV4). The manifest gate (when the caller supplied one) still
	// applies to the migrated result below, exactly like the v1/v2 paths.
	if (Save && Save->SchemaVersion == UProfileSaveGame::SchemaVersion3)
	{
		MigratedScratch = MigrateV3ToV4(*Save, OutError);
		if (!MigratedScratch)
		{
			return false;
		}
		Save = MigratedScratch;
	}'''),
    ('''	TArray<FRoomClearSaveEntry>* OutRoomClears)
{
	const UProfileSaveGame* Save = InSave;''',
'''	TArray<FRoomClearSaveEntry>* OutRoomClears,
	FProfessionStateOut* OutProfessionState)
{
	const UProfileSaveGame* Save = InSave;'''),
    ('''		*OutRoomClears = Save->RoomClears;
	}

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();''',
'''		*OutRoomClears = Save->RoomClears;
	}

	// M7-002: the restored profession state (a v4 save carries it verbatim; a
	// migrated v1/v2/v3 save carries the legacy-equivalent shape the migration
	// seeded). Never written on a failed load - this code only runs after
	// every step above succeeded.
	if (OutProfessionState)
	{
		FProfessionStateOut ProfessionState;
		ProfessionState.ProfessionId = Save->ProfessionId;
		ProfessionState.SkillRanks = Save->SkillRanks;
		ProfessionState.UnspentSP = Save->UnspentSP;
		ProfessionState.SkillLoadout = Save->SkillLoadout;
		ProfessionState.ProfessionConfigRevision = Save->ProfessionConfigRevision;
		ProfessionState.DungeonClears = Save->DungeonClears;
		*OutProfessionState = ProfessionState;
	}

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();'''),
    ('''UProfileSaveGame* FProfileSerializer::MigrateV2ToV3(const UProfileSaveGame& LegacySave,''',
'''UProfileSaveGame* FProfileSerializer::MigrateV3ToV4(const UProfileSaveGame& LegacySave,
	FProfileLoadError& OutError,
	UObject* Outer)
{
	// The migration is EXPLICITLY v3 -> v4: anything else is a caller bug or
	// an unknown schema, and the original save stays untouched either way.
	if (LegacySave.SchemaVersion == UProfileSaveGame::CurrentSchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("the save already carries schema_version %d; there is nothing to migrate (this channel accepts schema_version %d only)"),
			UProfileSaveGame::CurrentSchemaVersion, UProfileSaveGame::SchemaVersion3);
		return nullptr;
	}
	if (LegacySave.SchemaVersion != UProfileSaveGame::SchemaVersion3)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d is not the schema_version %d this migration accepts; refusing to load and refusing to modify or wipe the existing save file"),
			LegacySave.SchemaVersion, UProfileSaveGame::SchemaVersion3);
		return nullptr;
	}

	// The v3 payload must be a VALID v3 payload: the same field rules a load
	// applies. The manifest gate stays with the caller's post-migration load
	// validation - v3 -> v4 performs no content rewriting. A corrupt legacy
	// save is refused here - never partially migrated.
	if (!Validate(&LegacySave, OutError))
	{
		return nullptr;
	}

	// v4 adds ONLY the profession state, so every v3 field copies verbatim
	// and the new columns seed the legacy-equivalent shape (nothing stays
	// implicit - the same rule as the v1 weapon-state and v3 RoomClears
	// seeding): the prototype profession, the four legacy attacks at rank 1
	// (a legacy player is never locked out of the normal attacks, and the
	// legacy attacks do not consume MP or SP), UnspentSP 0, an EMPTY loadout,
	// no revision and DungeonClears 0.
	UProfileSaveGame* Migrated = NewObject<UProfileSaveGame>(Outer ? Outer : GetTransientPackage());
	Migrated->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
	Migrated->CharacterId = LegacySave.CharacterId;
	Migrated->Level = LegacySave.Level;
	Migrated->XP = LegacySave.XP;
	Migrated->MasterVolume = LegacySave.MasterVolume;
	Migrated->InventoryInstanceSnapshots = LegacySave.InventoryInstanceSnapshots;
	Migrated->EquippedIds = LegacySave.EquippedIds;
	Migrated->PendingRewards = LegacySave.PendingRewards;
	Migrated->AppliedSettlementIds = LegacySave.AppliedSettlementIds;
	Migrated->WeaponMagazines = LegacySave.WeaponMagazines;
	Migrated->ReserveAmmoPools = LegacySave.ReserveAmmoPools;
	Migrated->bWeaponStateInitialized = LegacySave.bWeaponStateInitialized;
	Migrated->ConfigRevision = LegacySave.ConfigRevision;
	Migrated->RoomClears = LegacySave.RoomClears;
	Migrated->ProfessionId = FName(TEXT("prototype"));
	for (const FName& LegacySkill : FProfessionSaveState::LegacyPrototypeSkillIds())
	{
		FSkillRankSaveEntry RankRow;
		RankRow.SkillId = LegacySkill;
		RankRow.Rank = 1;
		Migrated->SkillRanks.Add(RankRow);
	}
	Migrated->UnspentSP = 0;
	Migrated->SkillLoadout.Reset();
	Migrated->ProfessionConfigRevision.Reset();
	Migrated->DungeonClears = 0;

	// Defensive self-check: the migration's own output must validate as v4
	// (the profession-state rules included).
	FProfileLoadError SelfCheck;
	if (!Validate(Migrated, SelfCheck))
	{
		OutError.Code = SelfCheck.Code;
		OutError.Message = FString::Printf(TEXT("the migrated v4 save failed its own validation: %s"), *SelfCheck.Message);
		return nullptr;
	}
	return Migrated;
}

UProfileSaveGame* FProfileSerializer::MigrateV2ToV3(const UProfileSaveGame& LegacySave,'''),
    # v4 校验规则（room-clears 块后）
    ('''			SeenRoomIds.Add(Entry.RoomId);
		}

		return true;
	}
}''',
'''			SeenRoomIds.Add(Entry.RoomId);
		}

		// M7-002: the profession state (schema_version 4). Every rule is
		// structural - the profession's EXISTENCE against the live catalog is
		// the consuming layer's gate, this validator stays catalog-free.
		if (Save.SchemaVersion >= UProfileSaveGame::SchemaVersion3 + 1)
		{
			if (Save.ProfessionId.IsNone() || Save.ProfessionId.IsEqual(NAME_None, ENameCase::IgnoreCase)
				|| !IsValidDefinitionIdText(Save.ProfessionId.ToString()))
			{
				OutError.Code = EProfileLoadError::InvalidProfessionState;
				OutError.Message = FString::Printf(
					TEXT("ProfessionId must be a non-empty 1-64 character a-z0-9_ id (got '%s')"),
					*Save.ProfessionId.ToString());
				return false;
			}
			if (Save.UnspentSP < 0 || Save.DungeonClears < 0)
			{
				OutError.Code = EProfileLoadError::InvalidProfessionState;
				OutError.Message = FString::Printf(
					TEXT("UnspentSP (%d) and DungeonClears (%d) must be >= 0"),
					Save.UnspentSP, Save.DungeonClears);
				return false;
			}
			TSet<FName> SeenRankSkills;
			for (int32 Index = 0; Index < Save.SkillRanks.Num(); ++Index)
			{
				const FSkillRankSaveEntry& Entry = Save.SkillRanks[Index];
				if (Entry.SkillId.IsNone() || !IsValidDefinitionIdText(Entry.SkillId.ToString()))
				{
					OutError.Code = EProfileLoadError::InvalidProfessionState;
					OutError.Message = FString::Printf(
						TEXT("SkillRanks[%d].SkillId must be a non-empty 1-64 character a-z0-9_ id (got '%s')"),
						Index, *Entry.SkillId.ToString());
					return false;
				}
				if (Entry.Rank < 1 || Entry.Rank > 8)
				{
					OutError.Code = EProfileLoadError::InvalidProfessionState;
					OutError.Message = FString::Printf(
						TEXT("SkillRanks[%d] ('%s').Rank must be 1..8 (got %d)"),
						Index, *Entry.SkillId.ToString(), Entry.Rank);
					return false;
				}
				if (SeenRankSkills.Contains(Entry.SkillId))
				{
					OutError.Code = EProfileLoadError::DuplicateProfessionState;
					OutError.Message = FString::Printf(
						TEXT("SkillRanks[%d]: SkillId '%s' carries more than one rank row; duplicates are rejected, never silently merged"),
						Index, *Entry.SkillId.ToString());
					return false;
				}
				SeenRankSkills.Add(Entry.SkillId);
			}
			TSet<int32> SeenLoadoutSlots;
			for (int32 Index = 0; Index < Save.SkillLoadout.Num(); ++Index)
			{
				const FSkillSlotSaveEntry& Entry = Save.SkillLoadout[Index];
				if (Entry.SlotIndex < 1 || Entry.SlotIndex > 8)
				{
					OutError.Code = EProfileLoadError::InvalidProfessionState;
					OutError.Message = FString::Printf(
						TEXT("SkillLoadout[%d].SlotIndex must be 1..8 (got %d)"),
						Index, Entry.SlotIndex);
					return false;
				}
				if (Entry.SkillId.IsNone() || !IsValidDefinitionIdText(Entry.SkillId.ToString()))
				{
					OutError.Code = EProfileLoadError::InvalidProfessionState;
					OutError.Message = FString::Printf(
						TEXT("SkillLoadout[%d].SkillId must be a non-empty 1-64 character a-z0-9_ id (got '%s')"),
						Index, *Entry.SkillId.ToString());
					return false;
				}
				if (SeenLoadoutSlots.Contains(Entry.SlotIndex))
				{
					OutError.Code = EProfileLoadError::DuplicateProfessionState;
					OutError.Message = FString::Printf(
						TEXT("SkillLoadout[%d]: SlotIndex %d carries more than one assignment; duplicates are rejected, never silently merged"),
						Index, Entry.SlotIndex);
					return false;
				}
				SeenLoadoutSlots.Add(Entry.SlotIndex);
				if (!SeenRankSkills.Contains(Entry.SkillId))
				{
					OutError.Code = EProfileLoadError::DanglingLoadoutSkill;
					OutError.Message = FString::Printf(
						TEXT("SkillLoadout[%d]: skill '%s' has no learned rank row; a dangling hotbar assignment can never launch anything"),
						Index, *Entry.SkillId.ToString());
					return false;
				}
			}
		}

		return true;
	}
}'''),
])

apply('Source/UEMMO/Persistence/ProfileSaveService.h', [
    ('''#include "../Profile/RewardService.h"
#include "ProfileSaveGame.h"''',
'''#include "../Profile/RewardService.h"
#include "ProfileSaveGame.h"
#include "ProfileSerializer.h"'''),
    ('''	/** Per-room clear records at capture time (schema v3 field; empty = no clears yet). */
	TArray<FRoomClearSaveEntry> RoomClears;
};''',
'''	/** Per-room clear records at capture time (schema v3 field; empty = no clears yet). */
	TArray<FRoomClearSaveEntry> RoomClears;

	// -- M7-002: the profession state rides the same consistent snapshot ------

	/**
	 * The profession state at capture time (schema v4 fields). A request
	 * assembled without one carries the legacy-equivalent defaults (the
	 * serializer stamps them), so pre-existing request builders compile and
	 * produce a legal v4 payload unchanged.
	 */
	FProfessionSaveState ProfessionState;
};'''),
    ('''	// M6-003: the restored slot's per-room clear records (empty on v1/v2 slots
	// and on every failure - the migration seeds them explicitly empty).
	TArray<FRoomClearSaveEntry> RoomClears;
};''',
'''	// M6-003: the restored slot's per-room clear records (empty on v1/v2 slots
	// and on every failure - the migration seeds them explicitly empty).
	TArray<FRoomClearSaveEntry> RoomClears;

	// M7-002: the restored slot's profession state (the legacy-equivalent
	// shape on migrated v1/v2/v3 slots; default values on every failure).
	FProfessionStateOut ProfessionState;
};'''),
    ('''	/** Restored profile state; default (empty) values on any failure. */
	FProfileSnapshot Snapshot;
	FInventoryModel Inventory;
	TArray<FPendingReward> PendingRewards;
	TSet<uint64> AppliedSettlementIds;
	TMap<EItemSlot, FGuid> EquippedMap;
};''',
'''	/** Restored profile state; default (empty) values on any failure. */
	FProfileSnapshot Snapshot;
	FInventoryModel Inventory;
	TArray<FPendingReward> PendingRewards;
	TSet<uint64> AppliedSettlementIds;
	TMap<EItemSlot, FGuid> EquippedMap;

	// M6-003: the restored slot's per-room clear records (empty on v1/v2 slots).
	TArray<FRoomClearSaveEntry> RoomClears;

	// M7-002: the restored profession state (default values on any failure).
	FProfessionStateOut ProfessionState;
};'''),
    ('''		/** Restored data (valid only when bLoaded). */
		FProfileSnapshot Snapshot;
		FInventoryModel Inventory;
		TArray<FPendingReward> PendingRewards;
		TSet<uint64> AppliedSettlementIds;''',
'''		/** Restored data (valid only when bLoaded). */
		FProfileSnapshot Snapshot;
		FInventoryModel Inventory;
		TArray<FPendingReward> PendingRewards;
		TSet<uint64> AppliedSettlementIds;

		// M7-002: the restored profession state (default values unless loaded).
		FProfessionStateOut ProfessionState;'''),
])

apply('Source/UEMMO/Persistence/ProfileSaveService.cpp', [
    ('''	UProfileSaveGame* Payload = FProfileSerializer::ToSaveGame(Request.Snapshot, Request.Inventory,
		Request.PendingRewards, Request.AppliedSettlementIds, Request.EquippedMap,
		Request.WeaponMagazines, Request.ReserveAmmoPools, Request.bWeaponStateInitialized,
		Request.ConfigRevision, Request.RoomClears);''',
'''	UProfileSaveGame* Payload = FProfileSerializer::ToSaveGame(Request.Snapshot, Request.Inventory,
		Request.PendingRewards, Request.AppliedSettlementIds, Request.EquippedMap,
		Request.WeaponMagazines, Request.ReserveAmmoPools, Request.bWeaponStateInitialized,
		Request.ConfigRevision, Request.RoomClears, &Request.ProfessionState);'''),
    ('''		FProfileLoadError Error;
		if (!FProfileSerializer::FromSaveGame(Slot, Out.Snapshot, Out.Inventory, Out.PendingRewards,
			Out.AppliedSettlementIds, Out.EquippedMap, Error))
		{
			OutProblem = FString::Printf(TEXT("slot '%s' failed the serializer validation: %s"), *SlotNames[SlotIndex], *Error.Message);
			return false;
		}
		Out.Generation = Slot->SlotGeneration;''',
'''		FProfileLoadError Error;
		FProfessionStateOut ProfessionState;
		if (!FProfileSerializer::FromSaveGame(Slot, Out.Snapshot, Out.Inventory, Out.PendingRewards,
			Out.AppliedSettlementIds, Out.EquippedMap, Error, nullptr, nullptr, nullptr, nullptr,
			&ProfessionState))
		{
			OutProblem = FString::Printf(TEXT("slot '%s' failed the serializer validation: %s"), *SlotNames[SlotIndex], *Error.Message);
			return false;
		}
		// M7-002: the restored profession state rides the outcome (the
		// GameFlow restore pushes it into the profile's v4 fields).
		Out.ProfessionState = ProfessionState;
		Out.Generation = Slot->SlotGeneration;'''),
    ('''	FProfileLoadError Error;
	FProfileWeaponStateOut WeaponState;
	TArray<FRoomClearSaveEntry> RoomClears;
	if (!FProfileSerializer::FromSaveGame(Slot, OutRead.Snapshot, OutRead.Inventory, OutRead.PendingRewards,
		OutRead.AppliedSettlementIds, OutRead.EquippedMap, Error, nullptr, nullptr, &WeaponState, &RoomClears))
	{
		OutRead.Problem = FString::Printf(TEXT("the save of '%s' failed the serializer validation: %s"), *SlotNames[SlotIndex], *Error.Message);
		return;
	}
	OutRead.WeaponMagazines = WeaponState.WeaponMagazines;
	OutRead.ReserveAmmoPools = WeaponState.ReserveAmmoPools;
	OutRead.bWeaponStateInitialized = WeaponState.bWeaponStateInitialized;
	OutRead.WeaponConfigRevision = WeaponState.ConfigRevision;
	OutRead.RoomClears = MoveTemp(RoomClears);''',
'''	FProfileLoadError Error;
	FProfileWeaponStateOut WeaponState;
	TArray<FRoomClearSaveEntry> RoomClears;
	FProfessionStateOut ProfessionState;
	if (!FProfileSerializer::FromSaveGame(Slot, OutRead.Snapshot, OutRead.Inventory, OutRead.PendingRewards,
		OutRead.AppliedSettlementIds, OutRead.EquippedMap, Error, nullptr, nullptr, &WeaponState, &RoomClears,
		&ProfessionState))
	{
		OutRead.Problem = FString::Printf(TEXT("the save of '%s' failed the serializer validation: %s"), *SlotNames[SlotIndex], *Error.Message);
		return;
	}
	OutRead.WeaponMagazines = WeaponState.WeaponMagazines;
	OutRead.ReserveAmmoPools = WeaponState.ReserveAmmoPools;
	OutRead.bWeaponStateInitialized = WeaponState.bWeaponStateInitialized;
	OutRead.WeaponConfigRevision = WeaponState.ConfigRevision;
	OutRead.RoomClears = MoveTemp(RoomClears);
	// M7-002: the restored profession state travels with the restored data
	// (the legacy-equivalent shape on a migrated v1/v2/v3 slot).
	OutRead.ProfessionState = MoveTemp(ProfessionState);'''),
    ('''		// M6-003: the per-room clear records travel with the restored data.
		Outcome.RoomClears = Source.RoomClears;
	};''',
'''		// M6-003: the per-room clear records travel with the restored data.
		Outcome.RoomClears = Source.RoomClears;
		// M7-002: the restored profession state travels with the restored data.
		Outcome.ProfessionState = Source.ProfessionState;
	};'''),
    ('''	for (const FRoomClearSaveEntry& Clear : Save.RoomClears)
	{
		Canonical += FString::Printf(TEXT("room=%s:%d:%lld|"),
			*Clear.RoomId.ToString(), Clear.ClearCount, Clear.LastClearUnixSeconds);
	}
	return FCrc::StrCrc32(*Canonical);''',
'''	for (const FRoomClearSaveEntry& Clear : Save.RoomClears)
	{
		Canonical += FString::Printf(TEXT("room=%s:%d:%lld|"),
			*Clear.RoomId.ToString(), Clear.ClearCount, Clear.LastClearUnixSeconds);
	}
	// M7-002: the profession-state fields join the canonical form ONLY for a
	// schema_version 4 payload (the digest-coverage rule: a v1/v2/v3 payload
	// carries none of the new columns and must hash to EXACTLY its pre-M7
	// canonical string, so the migration routing and the planted-legacy-digest
	// tests keep running).
	if (Save.SchemaVersion >= UProfileSaveGame::SchemaVersion3 + 1)
	{
		Canonical += FString::Printf(TEXT("prof=%s|sp=%d|dc=%d|prev=%s|"),
			*Save.ProfessionId.ToString(), Save.UnspentSP, Save.DungeonClears,
			*Save.ProfessionConfigRevision);
		for (const FSkillRankSaveEntry& Rank : Save.SkillRanks)
		{
			Canonical += FString::Printf(TEXT("rank=%s:%d|"), *Rank.SkillId.ToString(), Rank.Rank);
		}
		for (const FSkillSlotSaveEntry& SlotEntry : Save.SkillLoadout)
		{
			Canonical += FString::Printf(TEXT("slot=%d:%s|"), SlotEntry.SlotIndex, *SlotEntry.SkillId.ToString());
		}
	}
	return FCrc::StrCrc32(*Canonical);'''),
])

apply('Source/UEMMO/Profile/RewardService.cpp', [
    ('''		// M6-003: the per-room clear records ride every claim-flow save the
		// same way (the in-memory profile is the restore of the last committed
		// save plus the BeginReward recordings made since - exactly the state
		// this persistence point must carry forward).
		Request.RoomClears = Profile.GetRoomClears();
		return Request;''',
'''		// M6-003: the per-room clear records ride every claim-flow save the
		// same way (the in-memory profile is the restore of the last committed
		// save plus the BeginReward recordings made since - exactly the state
		// this persistence point must carry forward).
		Request.RoomClears = Profile.GetRoomClears();
		// M7-002: the profession state rides every claim-flow save the same
		// way (the in-memory profession fields are the restore of the last
		// committed save plus any M7-003 rank purchases made since).
		Request.ProfessionState.ProfessionId = Profile.GetProfessionId();
		Request.ProfessionState.SkillRanks = Profile.GetSkillRanks();
		Request.ProfessionState.UnspentSP = Profile.GetUnspentSP();
		Request.ProfessionState.SkillLoadout = Profile.GetSkillLoadout();
		Request.ProfessionState.ProfessionConfigRevision = FString();
		Request.ProfessionState.DungeonClears = Profile.GetDungeonClears();
		return Request;'''),
])

# ============ ProfileSubsystem.h/.cpp ============
apply('Source/UEMMO/Profile/ProfileSubsystem.h', [
    ('''	/** Value copy of the backing inventory at snapshot time (insertion order). */
	FInventoryModel Inventory;
};''',
'''	/** Value copy of the backing inventory at snapshot time (insertion order). */
	FInventoryModel Inventory;

	/**
	 * M7-002: the character's profession id (a Data/Professions
	 * profession_id; "prototype" for the legacy shape). The snapshot never
	 * carries the live skill-rank table - the save/service layer passes the
	 * profession bundle explicitly (the same one-source-of-truth rule as the
	 * inventory above).
	 */
	FName ProfessionId;
};'''),
    ('''	bool RestoreFromSave(const FProfileSnapshot& Snapshot, const FInventoryModel& InInventory,
		const TArray<FPendingReward>& InPendingRewards, const TSet<uint64>& InAppliedSettlementIds,
		const TMap<EItemSlot, FGuid>& InEquippedMap,
		const TArray<FWeaponMagazineSaveEntry>& InWeaponMagazines,
		const TArray<FAmmoReserveSaveEntry>& InReserveAmmoPools,
		bool bInWeaponStateInitialized, const FString& InWeaponConfigRevision,
		const TArray<FRoomClearSaveEntry>& InRoomClears = TArray<FRoomClearSaveEntry>());''',
'''	bool RestoreFromSave(const FProfileSnapshot& Snapshot, const FInventoryModel& InInventory,
		const TArray<FPendingReward>& InPendingRewards, const TSet<uint64>& InAppliedSettlementIds,
		const TMap<EItemSlot, FGuid>& InEquippedMap,
		const TArray<FWeaponMagazineSaveEntry>& InWeaponMagazines,
		const TArray<FAmmoReserveSaveEntry>& InReserveAmmoPools,
		bool bInWeaponStateInitialized, const FString& InWeaponConfigRevision,
		const TArray<FRoomClearSaveEntry>& InRoomClears = TArray<FRoomClearSaveEntry>());

	/**
	 * M7-002: the v4 restore - everything the ten-argument form restores plus
	 * the loaded profession state (ProfessionId, learned ranks, UnspentSP,
	 * the recorded default loadout, the profession-config provenance and the
	 * completed-run counter), pushed into the profile's v4 fields. Same
	 * all-or-nothing guarantee: a base refusal changes nothing here either.
	 */
	bool RestoreFromSave(const FProfileSnapshot& Snapshot, const FInventoryModel& InInventory,
		const TArray<FPendingReward>& InPendingRewards, const TSet<uint64>& InAppliedSettlementIds,
		const TMap<EItemSlot, FGuid>& InEquippedMap,
		const TArray<FWeaponMagazineSaveEntry>& InWeaponMagazines,
		const TArray<FAmmoReserveSaveEntry>& InReserveAmmoPools,
		bool bInWeaponStateInitialized, const FString& InWeaponConfigRevision,
		const TArray<FRoomClearSaveEntry>& InRoomClears,
		const FName InProfessionId,
		const TArray<FSkillRankSaveEntry>& InSkillRanks,
		int32 InUnspentSP,
		const TArray<FSkillSlotSaveEntry>& InSkillLoadout,
		const FString& InProfessionConfigRevision,
		int32 InDungeonClears);'''),
    ('''	const TArray<FRoomClearSaveEntry>& GetRoomClears() const;''',
'''	const TArray<FRoomClearSaveEntry>& GetRoomClears() const;

	// -- M7-002: the profession state (read-only surface) -----------------------

	/**
	 * The active character's profession id; NAME_None before the first
	 * profile (NewProfile/RestoreFromSave stamp it). Read-only: profession
	 * changes happen only through the save/roster flows, never in combat.
	 */
	FName GetProfessionId() const { return ProfessionId; }

	/** Value copy of the learned skill ranks (empty before the first profile). */
	TArray<FSkillRankSaveEntry> GetSkillRanks() const { return SkillRanks; }

	/** Unspent skill points (0 before the first profile). */
	int32 GetUnspentSP() const { return UnspentSP; }

	/** Value copy of the recorded default hotbar assignments. */
	TArray<FSkillSlotSaveEntry> GetSkillLoadout() const { return SkillLoadout; }

	/** Completed continuous dungeon runs (0 before the first profile). */
	int32 GetDungeonClears() const { return DungeonClears; }'''),
    ('''	TArray<FRoomClearSaveEntry> RoomClears;

	/**
	 * Card-listed placeholder ("AliveIds")''',
'''	TArray<FRoomClearSaveEntry> RoomClears;

	// -- M7-002: the profession state (schema v4 fields) -------------------------

	/** The active character's profession id (prototype for the legacy shape). */
	UPROPERTY()
	FName ProfessionId;

	/** The learned skill ranks (skill id -> owned rank; set semantics). */
	UPROPERTY()
	TArray<FSkillRankSaveEntry> SkillRanks;

	/** Unspent skill points. */
	UPROPERTY()
	int32 UnspentSP = 0;

	/** The recorded default hotbar assignments (slot -> skill). */
	UPROPERTY()
	TArray<FSkillSlotSaveEntry> SkillLoadout;

	/** The profession-config provenance of the last save. */
	UPROPERTY()
	FString ProfessionConfigRevision;

	/** Completed continuous dungeon runs. */
	UPROPERTY()
	int32 DungeonClears = 0;

	/**
	 * Card-listed placeholder ("AliveIds")'''),
])

apply('Source/UEMMO/Profile/ProfileSubsystem.cpp', [
    ('''	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = CharacterId;
	Snapshot.Level = Level;
	Snapshot.XP = XP;''',
'''	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = CharacterId;
	Snapshot.Level = Level;
	Snapshot.XP = XP;
	// M7-002: the active profession rides the snapshot (the skill-rank table
	// stays out - the save/service layer passes it explicitly).
	Snapshot.ProfessionId = ProfessionId;'''),
    ('''void UProfileSubsystem::RecordRoomCleared(FName RoomId, int64 UnixSeconds)''',
'''bool UProfileSubsystem::RestoreFromSave(const FProfileSnapshot& Snapshot, const FInventoryModel& InInventory,
	const TArray<FPendingReward>& InPendingRewards, const TSet<uint64>& InAppliedSettlementIds,
	const TMap<EItemSlot, FGuid>& InEquippedMap,
	const TArray<FWeaponMagazineSaveEntry>& InWeaponMagazines,
	const TArray<FAmmoReserveSaveEntry>& InReserveAmmoPools,
	bool bInWeaponStateInitialized, const FString& InWeaponConfigRevision,
	const TArray<FRoomClearSaveEntry>& InRoomClears,
	const FName InProfessionId,
	const TArray<FSkillRankSaveEntry>& InSkillRanks,
	int32 InUnspentSP,
	const TArray<FSkillSlotSaveEntry>& InSkillLoadout,
	const FString& InProfessionConfigRevision,
	int32 InDungeonClears)
{
	// The v4 form restores the same base state first (the ten-argument body),
	// then pushes the loaded profession state on top - all-or-nothing
	// propagates (a base refusal changes nothing here either).
	if (!RestoreFromSave(Snapshot, InInventory, InPendingRewards, InAppliedSettlementIds, InEquippedMap,
		InWeaponMagazines, InReserveAmmoPools, bInWeaponStateInitialized, InWeaponConfigRevision, InRoomClears))
	{
		return false;
	}
	// M7-002: the loaded profession state restores verbatim (a v1/v2/v3 slot
	// hands in the migration's legacy-equivalent shape). The structural rules
	// (id syntax, rank range, set semantics) were validated by the serializer;
	// this entry is public, so the id is re-checked - a nameless profession
	// must never surface on a restored profile.
	if (InProfessionId.IsNone())
	{
		return false;
	}
	ProfessionId = InProfessionId;
	SkillRanks = InSkillRanks;
	UnspentSP = InUnspentSP;
	SkillLoadout = InSkillLoadout;
	ProfessionConfigRevision = InProfessionConfigRevision;
	DungeonClears = InDungeonClears;
	return true;
}

void UProfileSubsystem::RecordRoomCleared(FName RoomId, int64 UnixSeconds)'''),
])

apply('Source/UEMMO/Profile/GameFlowSubsystem.cpp', [
    ('''			bStartupProfileRestored = Profile->RestoreFromSave(
				StartupOutcome.Snapshot,
				StartupOutcome.Inventory,
				StartupOutcome.PendingRewards,
				StartupOutcome.AppliedSettlementIds,
				StartupOutcome.EquippedMap,
				StartupOutcome.WeaponMagazines,
				StartupOutcome.ReserveAmmoPools,
				StartupOutcome.bWeaponStateInitialized,
				StartupOutcome.WeaponConfigRevision,
				StartupOutcome.RoomClears);''',
'''			bStartupProfileRestored = Profile->RestoreFromSave(
				StartupOutcome.Snapshot,
				StartupOutcome.Inventory,
				StartupOutcome.PendingRewards,
				StartupOutcome.AppliedSettlementIds,
				StartupOutcome.EquippedMap,
				StartupOutcome.WeaponMagazines,
				StartupOutcome.ReserveAmmoPools,
				StartupOutcome.bWeaponStateInitialized,
				StartupOutcome.WeaponConfigRevision,
				StartupOutcome.RoomClears,
				StartupOutcome.ProfessionState.ProfessionId,
				StartupOutcome.ProfessionState.SkillRanks,
				StartupOutcome.ProfessionState.UnspentSP,
				StartupOutcome.ProfessionState.SkillLoadout,
				StartupOutcome.ProfessionState.ProfessionConfigRevision,
				StartupOutcome.ProfessionState.DungeonClears);'''),
])

print('ALL M7-002 EDITS APPLIED')
