// M3-013: implementation of the pure profile save serializer. No World, no
// subsystem, no file I/O: ToSaveGame maps trusted in-memory profile state into
// a fresh UProfileSaveGame; Validate accepts/rejects a save against the
// schema rules (M5-046: versions 1 and 2 - version 1 loads through the
// explicit migration below); FromSaveGame restores into caller-owned
// containers only after validation passed, and never touches any Out*
// parameter on the error path (a rejected load can never surface as a
// half-restored profile or a silently dropped item).
//
// M5-046 additions: the legacy_item_aliases manifest (explicit, data-driven
// accessory_training -> charm_training history mapping; unknown ids are
// REJECTED, never silently aliased), the one-time v1 -> v2 weapon-state
// migration (per-instance magazines seeded empty, identity fully preserved)
// and the ConfigRevision compatibility classifier (saved weapon state vs the
// current ammo config - the revision strings themselves are never compared).

#include "ProfileSerializer.h"

#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"

#include "UObject/UObjectGlobals.h"

UProfileSaveGame* FProfileSerializer::ToSaveGame(const FProfileSnapshot& Snapshot,
	const FInventoryModel& Inventory,
	const TArray<FPendingReward>& PendingRewards,
	const TSet<uint64>& AppliedSettlementIds,
	const TMap<EItemSlot, FGuid>& EquippedMap,
	const TArray<FWeaponMagazineSaveEntry>& WeaponMagazines,
	const TArray<FAmmoReserveSaveEntry>& ReserveAmmoPools,
	bool bInWeaponStateInitialized,
	const FString& InConfigRevision,
	const TArray<FRoomClearSaveEntry>& InRoomClears,
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

	// Inventory instances as their M3-001 snapshot strings, insertion order.
	for (const FItemInstance& Instance : Inventory.GetAll())
	{
		Save->InventoryInstanceSnapshots.Add(Instance.ToStringSnapshot());
	}

	// TMap<EItemSlot, FGuid> as an array of slot/InstanceId pairs.
	for (const TPair<EItemSlot, FGuid>& Pair : EquippedMap)
	{
		FEquippedSlotSavePair SavePair;
		SavePair.Slot = static_cast<uint8>(Pair.Key);
		SavePair.InstanceId = Pair.Value;
		Save->EquippedIds.Add(SavePair);
	}

	// Pending drafts: value fields verbatim, items as snapshot strings (the
	// frozen roll travels inside the string; a load never re-rolls it).
	for (const FPendingReward& Draft : PendingRewards)
	{
		FPendingRewardSaveEntry Entry;
		Entry.SettlementId = Draft.SettlementId;
		Entry.XP = Draft.XP;
		for (const FItemInstance& Item : Draft.Items)
		{
			Entry.ItemSnapshots.Add(Item.ToStringSnapshot());
		}
		Save->PendingRewards.Add(Entry);
	}

	// TSet<uint64> as a plain array; set semantics are restored on load.
	Save->AppliedSettlementIds = AppliedSettlementIds.Array();

	// M5-046: the weapon state travels verbatim (rows are plain value rows;
	// the caller owns their consistency, the load path re-validates them).
	Save->WeaponMagazines = WeaponMagazines;
	Save->ReserveAmmoPools = ReserveAmmoPools;
	Save->bWeaponStateInitialized = bInWeaponStateInitialized;
	Save->ConfigRevision = InConfigRevision;

	// M6-003: the per-room clear records travel verbatim (plain value rows;
	// the caller owns their consistency, the load path re-validates them).
	Save->RoomClears = InRoomClears;

	return Save;
}

// -- M5-046: the build-wide item manifest ----------------------------------------

bool FProfileItemManifest::Validate(FString& OutProblem) const
{
	// Aliases: distinct non-none ids, no duplicate legacy rows, no legacy id
	// doubling as a current definition, target present among the known ids.
	TSet<FName> SeenLegacyIds;
	for (int32 Index = 0; Index < LegacyAliases.Num(); ++Index)
	{
		const FLegacyItemAliasEntry& Alias = LegacyAliases[Index];
		if (Alias.LegacyId.IsNone() || Alias.CurrentId.IsNone())
		{
			OutProblem = FString::Printf(TEXT("LegacyAliases[%d]: both LegacyId and CurrentId must be non-empty identifiers"), Index);
			return false;
		}
		if (Alias.LegacyId == Alias.CurrentId)
		{
			OutProblem = FString::Printf(TEXT("LegacyAliases[%d]: '%s' aliases itself; a self-alias resolves nothing"),
				Index, *Alias.LegacyId.ToString());
			return false;
		}
		if (SeenLegacyIds.Contains(Alias.LegacyId))
		{
			OutProblem = FString::Printf(TEXT("LegacyAliases[%d]: legacy id '%s' is mapped more than once"),
				Index, *Alias.LegacyId.ToString());
			return false;
		}
		SeenLegacyIds.Add(Alias.LegacyId);
		if (IsKnownDefinition(Alias.LegacyId))
		{
			OutProblem = FString::Printf(TEXT("LegacyAliases[%d]: legacy id '%s' is also a known current definition; the two vocabularies must stay disjoint"),
				Index, *Alias.LegacyId.ToString());
			return false;
		}
		if (!IsKnownDefinition(Alias.CurrentId))
		{
			OutProblem = FString::Printf(TEXT("LegacyAliases[%d]: alias target '%s' is not a known current definition; the mapping would resolve into the unknown"),
				Index, *Alias.CurrentId.ToString());
			return false;
		}
	}

	// Acyclicity: follow each legacy chain (legacy -> current, and if the
	// current id is itself a legacy source, continue) with a visited set - a
	// revisit means the graph loops and no id would ever resolve to a known
	// definition.
	for (const FLegacyItemAliasEntry& Alias : LegacyAliases)
	{
		TSet<FName> Visited;
		FName Current = Alias.LegacyId;
		while (true)
		{
			if (Visited.Contains(Current))
			{
				OutProblem = FString::Printf(TEXT("LegacyAliases: the alias graph starting at '%s' contains a cycle; a chained alias would never resolve"),
					*Alias.LegacyId.ToString());
				return false;
			}
			Visited.Add(Current);
			const FLegacyItemAliasEntry* Next = nullptr;
			for (const FLegacyItemAliasEntry& Candidate : LegacyAliases)
			{
				if (Candidate.LegacyId == Current)
				{
					Next = &Candidate;
					break;
				}
			}
			if (!Next)
			{
				break;
			}
			Current = Next->CurrentId;
		}
	}

	// Seeds: known item definitions only, valid ammo id text, a capacity the
	// magazine model would accept, no duplicate rows.
	TSet<FName> SeenSeedDefinitions;
	for (const TPair<FName, FWeaponAmmoSeed>& Seed : WeaponAmmoSeeds)
	{
		if (!IsKnownDefinition(Seed.Key))
		{
			OutProblem = FString::Printf(TEXT("WeaponAmmoSeeds: seed item definition '%s' is not a known current definition"),
				*Seed.Key.ToString());
			return false;
		}
		if (SeenSeedDefinitions.Contains(Seed.Key))
		{
			OutProblem = FString::Printf(TEXT("WeaponAmmoSeeds: item definition '%s' carries more than one seed row"),
				*Seed.Key.ToString());
			return false;
		}
		SeenSeedDefinitions.Add(Seed.Key);
		if (!IsValidDefinitionIdText(Seed.Value.AmmoId.ToString()))
		{
			OutProblem = FString::Printf(TEXT("WeaponAmmoSeeds['%s']: AmmoId must be 1-64 characters of a-z0-9_ (got '%s')"),
				*Seed.Key.ToString(), *Seed.Value.AmmoId.ToString());
			return false;
		}
		if (Seed.Value.MagazineCapacity < 1 || Seed.Value.MagazineCapacity > MaxAmmoMagazineSize)
		{
			OutProblem = FString::Printf(TEXT("WeaponAmmoSeeds['%s']: MagazineCapacity must be 1..%d (got %d)"),
				*Seed.Key.ToString(), MaxAmmoMagazineSize, Seed.Value.MagazineCapacity);
			return false;
		}
	}

	OutProblem.Reset();
	return true;
}

bool FProfileItemManifest::IsKnownDefinition(FName Id) const
{
	return KnownDefinitionIds.Contains(Id);
}

bool FProfileItemManifest::ResolveDefinitionId(FName SavedId, FName& OutCurrentId) const
{
	if (IsKnownDefinition(SavedId))
	{
		OutCurrentId = SavedId;
		return true;
	}
	for (const FLegacyItemAliasEntry& Alias : LegacyAliases)
	{
		if (Alias.LegacyId == SavedId)
		{
			OutCurrentId = Alias.CurrentId;
			return true;
		}
	}
	return false;
}

const FProfileItemManifest& FProfileItemManifest::Get()
{
	static const FProfileItemManifest Manifest = []()
	{
		FProfileItemManifest Built;
		// The shipped Data/items.json vocabulary (M5-020A normalization):
		// every definition id a legitimate save of this build may reference.
		Built.KnownDefinitionIds = {
			FName(TEXT("weapon_training")),
			FName(TEXT("weapon_hitscan_gun")),
			FName(TEXT("weapon_burst_gun")),
			FName(TEXT("weapon_pellet_gun")),
			FName(TEXT("weapon_rifle")),
			FName(TEXT("weapon_grenade_launcher")),
			FName(TEXT("weapon_mortar")),
			FName(TEXT("weapon_tracker")),
			FName(TEXT("armor_training")),
			FName(TEXT("charm_training"))
		};
		// The frozen section-10.9 alias: the M3-011 HUD debug staging minted
		// instances under accessory_training before items.json standardized
		// the kind as charm_training. Those saves are legal history, not
		// corruption; the manifest is the explicit mapping that carries them.
		FLegacyItemAliasEntry CharmAlias;
		CharmAlias.LegacyId = FName(TEXT("accessory_training"));
		CharmAlias.CurrentId = FName(TEXT("charm_training"));
		Built.LegacyAliases.Add(CharmAlias);

		// The one-time weapon-state seeds, transcribed from
		// Data/CombatSystem/weapons.json (magazine_size / ammo_id) through the
		// items.json weapon_definition_id mapping. weapon_training is melee
		// (weapon_training_sword) and deliberately carries NO row.
		const FWeaponAmmoSeed Seeds[] = {
			{ FName(TEXT("ammo_cell")), 12 },    // weapon_hitscan_gun -> weapon_hitscan_sample
			{ FName(TEXT("ammo_cell")), 15 },    // weapon_burst_gun   -> weapon_burst_sample
			{ FName(TEXT("ammo_cell")), 6 },     // weapon_pellet_gun  -> weapon_pellet_sample
			{ FName(TEXT("ammo_light")), 12 },   // weapon_rifle       -> weapon_projectile_sample
			{ FName(TEXT("ammo_rocket")), 3 },   // weapon_grenade_launcher -> weapon_explosive_sample
			{ FName(TEXT("ammo_shell")), 4 },    // weapon_mortar      -> weapon_parabolic_sample
			{ FName(TEXT("ammo_missile")), 3 }   // weapon_tracker     -> weapon_homing_sample
		};
		const FName SeedDefinitions[] = {
			FName(TEXT("weapon_hitscan_gun")),
			FName(TEXT("weapon_burst_gun")),
			FName(TEXT("weapon_pellet_gun")),
			FName(TEXT("weapon_rifle")),
			FName(TEXT("weapon_grenade_launcher")),
			FName(TEXT("weapon_mortar")),
			FName(TEXT("weapon_tracker"))
		};
		static_assert(sizeof(Seeds) / sizeof(Seeds[0]) == sizeof(SeedDefinitions) / sizeof(SeedDefinitions[0]),
			"the seed table and its definition ids must stay paired");
		for (int32 Index = 0; Index < sizeof(Seeds) / sizeof(Seeds[0]); ++Index)
		{
			Built.WeaponAmmoSeeds.Add(SeedDefinitions[Index], Seeds[Index]);
		}
		return Built;
	}();
	return Manifest;
}

// -- M5-046: internal helpers ------------------------------------------------------

namespace
{
	/**
	 * Parses one stored instance snapshot and, when a manifest is supplied,
	 * resolves its DefinitionId through the manifest. OutInstance carries the
	 * parsed (and possibly alias-rewritten) instance; OutSnapshotText carries
	 * the string to keep for storage - the ORIGINAL text untouched for a
	 * non-aliased id (byte preservation), a re-emitted snapshot for a mapped
	 * legacy id (the identity fields travel through the parse/emit round trip
	 * value-exactly). Returns false with a field-naming reason on any parse
	 * failure or unresolvable definition (the caller rejects the load).
	 */
	bool M5_046_ResolveInstanceSnapshot(const FString& SnapshotText, const FProfileItemManifest* ItemManifest,
		FItemInstance& OutInstance, FString& OutSnapshotText, FString& OutError)
	{
		FItemInstance Parsed;
		FString ParseError;
		if (!Parsed.FromStringSnapshot(SnapshotText, &ParseError))
		{
			OutError = ParseError;
			return false;
		}
		OutSnapshotText = SnapshotText;
		if (ItemManifest)
		{
			FName CurrentId;
			if (!ItemManifest->ResolveDefinitionId(Parsed.DefinitionId, CurrentId))
			{
				OutError = FString::Printf(
					TEXT("definition_id '%s' is neither a definition this build knows nor a registered legacy alias (registered history such as accessory_training resolves through the manifest; unknown ids are refused instead of silently kept)"),
					*Parsed.DefinitionId.ToString());
				return false;
			}
			if (CurrentId != Parsed.DefinitionId)
			{
				// Legacy alias: rewrite the definition id onto the current
				// kind. InstanceId/RollSeed/Level/RolledStats travel through
				// the round trip untouched - no re-roll, no re-forge.
				Parsed.DefinitionId = CurrentId;
				OutSnapshotText = Parsed.ToStringSnapshot();
			}
		}
		OutInstance = Parsed;
		return true;
	}

	/**
	 * The shared field-level validation body (the schema-version gate already
	 * ran). Checks the M3-013 rules plus the M5-046 v2 weapon-state rows and
	 * the M6-003 v3 room-clear rows; a v1/v2 save passes the newer checks
	 * trivially (its newer containers are empty). With a manifest supplied,
	 * every DefinitionId must resolve through it. Returns false with OutError
	 * naming the field.
	 */
	bool M5_046_ValidateBody(const UProfileSaveGame& Save, const FProfileItemManifest* ItemManifest,
		FProfileLoadError& OutError)
	{
		// Progression: the closed 1..MaxLevel range (Level 0 means "no profile",
		// which is never a legal on-disk state).
		if (Save.Level < 1 || Save.Level > UProfileSubsystem::MaxLevel)
		{
			OutError.Code = EProfileLoadError::InvalidLevel;
			OutError.Message = FString::Printf(
				TEXT("Level %d is outside the closed range 1..%d"),
				Save.Level, UProfileSubsystem::MaxLevel);
			return false;
		}

		// The settings placeholder float must be finite.
		if (!FMath::IsFinite(Save.MasterVolume))
		{
			OutError.Code = EProfileLoadError::NonFiniteSetting;
			OutError.Message = TEXT("MasterVolume must be a finite number");
			return false;
		}

		// Capacity: a save claiming more items than the inventory can hold is
		// corrupt; the overflow must be refused, never silently dropped.
		if (Save.InventoryInstanceSnapshots.Num() > FInventoryModel::Capacity)
		{
			OutError.Code = EProfileLoadError::InventoryOverfull;
			OutError.Message = FString::Printf(
				TEXT("InventoryInstances carries %d snapshot strings, more than the capacity %d; refusing to silently drop the overflow"),
				Save.InventoryInstanceSnapshots.Num(), FInventoryModel::Capacity);
			return false;
		}

		// Parse every item snapshot and enforce GLOBAL instance-id uniqueness
		// across the inventory AND all pending drafts. FromStringSnapshot itself
		// rejects invalid guids, duplicate/unknown fields and non-finite (NaN/Inf)
		// stats with a field-naming error, which is surfaced verbatim here.
		// InventoryIds (inventory only) is kept separate from the global set
		// because equipment and magazines may reference inventory instances only.
		TSet<FGuid> InventoryIds;
		TSet<FGuid> AllInstanceIds;
		for (int32 Index = 0; Index < Save.InventoryInstanceSnapshots.Num(); ++Index)
		{
			FItemInstance Instance;
			FString SnapshotText;
			FString ResolveError;
			if (!M5_046_ResolveInstanceSnapshot(Save.InventoryInstanceSnapshots[Index], ItemManifest,
				Instance, SnapshotText, ResolveError))
			{
				OutError.Code = EProfileLoadError::BadInstanceSnapshot;
				if (ResolveError.Contains(TEXT("definition_id")))
				{
					// The manifest refusal keeps its own classification: an
					// unknown-and-unregistered definition is content the save
					// contract does not know, not a malformed string.
					OutError.Code = EProfileLoadError::UnknownItemDefinition;
				}
				OutError.Message = FString::Printf(
					TEXT("InventoryInstances[%d]: %s"), Index, *ResolveError);
				return false;
			}
			if (AllInstanceIds.Contains(Instance.InstanceId))
			{
				OutError.Code = EProfileLoadError::DuplicateInstanceId;
				OutError.Message = FString::Printf(
					TEXT("InventoryInstances[%d]: InstanceId '%s' appears more than once; duplicate instances are rejected"),
					Index, *Instance.InstanceId.ToString());
				return false;
			}
			InventoryIds.Add(Instance.InstanceId);
			AllInstanceIds.Add(Instance.InstanceId);
		}

		for (int32 DraftIndex = 0; DraftIndex < Save.PendingRewards.Num(); ++DraftIndex)
		{
			const FPendingRewardSaveEntry& Entry = Save.PendingRewards[DraftIndex];
			for (int32 ItemIndex = 0; ItemIndex < Entry.ItemSnapshots.Num(); ++ItemIndex)
			{
				FItemInstance Item;
				FString SnapshotText;
				FString ResolveError;
				if (!M5_046_ResolveInstanceSnapshot(Entry.ItemSnapshots[ItemIndex], ItemManifest,
					Item, SnapshotText, ResolveError))
				{
					OutError.Code = ResolveError.Contains(TEXT("definition_id"))
						? EProfileLoadError::UnknownItemDefinition
						: EProfileLoadError::BadInstanceSnapshot;
					OutError.Message = FString::Printf(
						TEXT("PendingRewards[%d].Items[%d]: %s"),
						DraftIndex, ItemIndex, *ResolveError);
					return false;
				}
				if (AllInstanceIds.Contains(Item.InstanceId))
				{
					OutError.Code = EProfileLoadError::DuplicateInstanceId;
					OutError.Message = FString::Printf(
						TEXT("PendingRewards[%d].Items[%d]: InstanceId '%s' duplicates an already stored instance"),
						DraftIndex, ItemIndex, *Item.InstanceId.ToString());
					return false;
				}
				AllInstanceIds.Add(Item.InstanceId);
			}
		}

		// Equipment: closed slot bytes, unique bindings, and every InstanceId must
		// exist among the INVENTORY instances (interface contract section 8:
		// "Slot->InstanceId, only references inventory instances" - a pending
		// draft item is not a legal equip target, and a missing id is dangling).
		TSet<uint8> SeenSlots;
		for (int32 Index = 0; Index < Save.EquippedIds.Num(); ++Index)
		{
			const FEquippedSlotSavePair& Pair = Save.EquippedIds[Index];
			if (!IsValidItemSlot(static_cast<EItemSlot>(Pair.Slot)))
			{
				OutError.Code = EProfileLoadError::InvalidEquipSlot;
				OutError.Message = FString::Printf(
					TEXT("EquippedIds[%d].Slot %d is not one of the closed slots Weapon(0)/Armor(1)/Accessory(2)"),
					Index, static_cast<int32>(Pair.Slot));
				return false;
			}
			if (SeenSlots.Contains(Pair.Slot))
			{
				OutError.Code = EProfileLoadError::DuplicateEquipSlot;
				OutError.Message = FString::Printf(
					TEXT("EquippedIds: slot %d is bound more than once; a binding would be silently dropped"),
					static_cast<int32>(Pair.Slot));
				return false;
			}
			SeenSlots.Add(Pair.Slot);
			if (!InventoryIds.Contains(Pair.InstanceId))
			{
				OutError.Code = EProfileLoadError::DanglingEquipReference;
				OutError.Message = FString::Printf(
					TEXT("EquippedIds[%d]: equipped InstanceId '%s' does not exist in the inventory; dangling references are rejected"),
					Index, *Pair.InstanceId.ToString());
				return false;
			}
		}

		// M5-046: the per-instance magazine rows. Every field inside its
		// closed range, the identity unique and inventory-backed - the load
		// must never invent, clamp or silently drop magazine state.
		TSet<FGuid> SeenMagazineInstances;
		for (int32 Index = 0; Index < Save.WeaponMagazines.Num(); ++Index)
		{
			const FWeaponMagazineSaveEntry& Magazine = Save.WeaponMagazines[Index];
			if (!Magazine.InstanceId.IsValid())
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("WeaponMagazines[%d].InstanceId is the all-zero guid; a magazine needs a real item instance identity"),
					Index);
				return false;
			}
			if (!IsValidDefinitionIdText(Magazine.AmmoId.ToString()))
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("WeaponMagazines[%d].AmmoId must be 1-64 characters of a-z0-9_ (got '%s')"),
					Index, *Magazine.AmmoId.ToString());
				return false;
			}
			if (Magazine.Capacity < 1 || Magazine.Capacity > MaxAmmoMagazineSize)
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("WeaponMagazines[%d].Capacity must be 1..%d (got %d); a melee weapon carries no magazine row at all"),
					Index, MaxAmmoMagazineSize, Magazine.Capacity);
				return false;
			}
			if (Magazine.LoadedRounds < 0 || Magazine.LoadedRounds > Magazine.Capacity)
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("WeaponMagazines[%d].LoadedRounds must be 0..Capacity %d (got %d)"),
					Index, Magazine.Capacity, Magazine.LoadedRounds);
				return false;
			}
			if (SeenMagazineInstances.Contains(Magazine.InstanceId))
			{
				OutError.Code = EProfileLoadError::DuplicateWeaponState;
				OutError.Message = FString::Printf(
					TEXT("WeaponMagazines[%d]: InstanceId '%s' carries more than one magazine row; a duplicate would be silently dropped"),
					Index, *Magazine.InstanceId.ToString());
				return false;
			}
			SeenMagazineInstances.Add(Magazine.InstanceId);
			if (!InventoryIds.Contains(Magazine.InstanceId))
			{
				OutError.Code = EProfileLoadError::DanglingMagazineReference;
				OutError.Message = FString::Printf(
					TEXT("WeaponMagazines[%d]: magazine InstanceId '%s' does not exist in the inventory; an orphaned magazine is rejected instead of restored"),
					Index, *Magazine.InstanceId.ToString());
				return false;
			}
		}

		// M5-046: the shared reserve pools - unique per ammo kind, self-
		// consistent rounds/cap, inside the global reserve bound.
		TSet<FName> SeenPoolIds;
		for (int32 Index = 0; Index < Save.ReserveAmmoPools.Num(); ++Index)
		{
			const FAmmoReserveSaveEntry& Pool = Save.ReserveAmmoPools[Index];
			if (!IsValidDefinitionIdText(Pool.AmmoId.ToString()))
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("ReserveAmmoPools[%d].AmmoId must be 1-64 characters of a-z0-9_ (got '%s')"),
					Index, *Pool.AmmoId.ToString());
				return false;
			}
			if (Pool.MaxReserve < 0 || Pool.MaxReserve > MaxAmmoReserve)
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("ReserveAmmoPools[%d].MaxReserve must be 0..%d (got %d)"),
					Index, MaxAmmoReserve, Pool.MaxReserve);
				return false;
			}
			if (Pool.Rounds < 0 || Pool.Rounds > Pool.MaxReserve)
			{
				OutError.Code = EProfileLoadError::InvalidWeaponState;
				OutError.Message = FString::Printf(
					TEXT("ReserveAmmoPools[%d].Rounds must be 0..MaxReserve %d (got %d)"),
					Index, Pool.MaxReserve, Pool.Rounds);
				return false;
			}
			if (SeenPoolIds.Contains(Pool.AmmoId))
			{
				OutError.Code = EProfileLoadError::DuplicateWeaponState;
				OutError.Message = FString::Printf(
					TEXT("ReserveAmmoPools[%d]: AmmoId '%s' carries more than one pool row; a duplicate would be silently dropped"),
					Index, *Pool.AmmoId.ToString());
				return false;
			}
			SeenPoolIds.Add(Pool.AmmoId);
		}

		// M6-003: the per-room clear records - non-empty room ids, a positive
		// clear count, a non-negative timestamp and set semantics (one row per
		// room; duplicates would be silently deduplicated on restore).
		TSet<FName> SeenRoomIds;
		for (int32 Index = 0; Index < Save.RoomClears.Num(); ++Index)
		{
			const FRoomClearSaveEntry& Entry = Save.RoomClears[Index];
			if (Entry.RoomId.IsNone() || Entry.RoomId.IsEqual(NAME_None, ENameCase::IgnoreCase))
			{
				OutError.Code = EProfileLoadError::InvalidRoomClearRecord;
				OutError.Message = FString::Printf(
					TEXT("RoomClears[%d].RoomId must be a non-empty room id"),
					Index);
				return false;
			}
			if (Entry.ClearCount < 1)
			{
				OutError.Code = EProfileLoadError::InvalidRoomClearRecord;
				OutError.Message = FString::Printf(
					TEXT("RoomClears[%d] ('%s').ClearCount must be >= 1 (got %d); a room with no clear carries no row"),
					Index, *Entry.RoomId.ToString(), Entry.ClearCount);
				return false;
			}
			if (Entry.LastClearUnixSeconds < 0)
			{
				OutError.Code = EProfileLoadError::InvalidRoomClearRecord;
				OutError.Message = FString::Printf(
					TEXT("RoomClears[%d] ('%s').LastClearUnixSeconds must be >= 0 (got %lld)"),
					Index, *Entry.RoomId.ToString(), Entry.LastClearUnixSeconds);
				return false;
			}
			if (SeenRoomIds.Contains(Entry.RoomId))
			{
				OutError.Code = EProfileLoadError::DuplicateRoomClearRecord;
				OutError.Message = FString::Printf(
					TEXT("RoomClears[%d]: RoomId '%s' carries more than one record row; duplicates are rejected, never silently merged"),
					Index, *Entry.RoomId.ToString());
				return false;
			}
			SeenRoomIds.Add(Entry.RoomId);
		}

		return true;
	}
}

bool FProfileSerializer::Validate(const UProfileSaveGame* Save, FProfileLoadError& OutError,
	const FProfileItemManifest* ItemManifest)
{
	if (!Save)
	{
		OutError.Code = EProfileLoadError::NullSaveGame;
		OutError.Message = TEXT("SaveGame object is null; nothing to load");
		return false;
	}

	// Schema gate: anything this build does not know is refused - a version
	// above CurrentSchemaVersion belongs to a NEWER build, so the load must
	// fail and the original file must stay exactly as it is (no auto-wipe).
	// M5-046: schema version 1 is NOT refused here - it is the legacy layout
	// and loads through the explicit migration channel (FromSaveGame /
	// MigrateV1ToV2). M6-003: schema version 2 is likewise loadable through
	// the second migration channel (MigrateV2ToV3).
	if (Save->SchemaVersion > UProfileSaveGame::CurrentSchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d was written by a newer game build (this build understands schema_version %d); refusing to load and refusing to modify or wipe the existing save file"),
			Save->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
		return false;
	}
	if (Save->SchemaVersion != UProfileSaveGame::CurrentSchemaVersion
		&& Save->SchemaVersion != UProfileSaveGame::SchemaVersion2
		&& Save->SchemaVersion != UProfileSaveGame::LegacySchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d is not a schema this build knows (expected %d, or the migratable %d/%d); refusing to load and refusing to modify or wipe the existing save file"),
			Save->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion,
			UProfileSaveGame::SchemaVersion2, UProfileSaveGame::LegacySchemaVersion);
		return false;
	}

	if (!M5_046_ValidateBody(*Save, ItemManifest, OutError))
	{
		return false;
	}

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return true;
}

bool FProfileSerializer::FromSaveGame(const UProfileSaveGame* InSave,
	FProfileSnapshot& OutSnapshot,
	FInventoryModel& OutInventory,
	TArray<FPendingReward>& OutPendingRewards,
	TSet<uint64>& OutAppliedSettlementIds,
	TMap<EItemSlot, FGuid>& OutEquippedMap,
	FProfileLoadError& OutError,
	const FProfileItemManifest* ItemManifest,
	const TMap<FName, FAmmoType>* CurrentAmmoTypes,
	FProfileWeaponStateOut* OutWeaponState,
	TArray<FRoomClearSaveEntry>* OutRoomClears)
{
	const UProfileSaveGame* Save = InSave;

	// M5-046: a legacy schema_version 1 save enters through the explicit
	// migration channel. The migration produces a scratch v2 save (the input
	// object is never modified) and validates it under the same field rules;
	// without a caller-supplied manifest the build-wide default applies (a
	// real v1 save references real content ids, which the default manifest
	// knows). Every later step works on the migrated save uniformly.
	UProfileSaveGame* MigratedScratch = nullptr;
	const FProfileItemManifest* ActiveManifest = ItemManifest;
	if (Save && Save->SchemaVersion == UProfileSaveGame::LegacySchemaVersion)
	{
		if (!ActiveManifest)
		{
			ActiveManifest = &FProfileItemManifest::Get();
		}
		MigratedScratch = MigrateV1ToV2(*Save, *ActiveManifest, OutError);
		if (!MigratedScratch)
		{
			return false;
		}
		Save = MigratedScratch;
	}

	// M6-003: a schema_version 2 save enters through the second migration
	// channel - a verbatim v3 copy whose RoomClears seed explicitly empty.
	// The manifest gate (when the caller supplied one) still applies to the
	// migrated result below, exactly like the v1 path.
	if (Save && Save->SchemaVersion == UProfileSaveGame::SchemaVersion2)
	{
		MigratedScratch = MigrateV2ToV3(*Save, OutError);
		if (!MigratedScratch)
		{
			return false;
		}
		Save = MigratedScratch;
	}

	// Single validation gate; on failure no Out* parameter is written.
	if (!Validate(Save, OutError, ActiveManifest))
	{
		return false;
	}

	// M5-046: the ConfigRevision compatibility gate. The revision STRINGS are
	// never compared (provenance only); when the caller supplies the current
	// ammo config, a saved weapon state the current config can no longer hold
	// refuses the load here - the original save stays untouched.
	if (CurrentAmmoTypes && !IsWeaponStateCompatibleWithAmmoConfig(*Save, *CurrentAmmoTypes, OutError))
	{
		return false;
	}

	// Restore into locals first, commit only after every step succeeded.
	// Validate already guarantees parse success, unique ids and capacity, so
	// the guards below are defensive - but they must fail loudly instead of
	// silently dropping anything.
	FProfileSnapshot Restored;
	Restored.CharacterId = Save->CharacterId;
	Restored.Level = Save->Level;
	Restored.XP = Save->XP;
	// Derived stats are never stored in the save: they are recomputed from
	// the level formulas (the pre-equipment base row). The full equipment sum
	// is re-derived by the gameplay layer once EquippedIds are re-applied.
	Restored.MaxHP = UProfileSubsystem::GetMaxHPForLevel(Restored.Level);
	Restored.Attack = UProfileSubsystem::GetAttackForLevel(Restored.Level);
	Restored.Defense = UProfileSubsystem::GetDefenseForLevel(Restored.Level);

	FInventoryModel RestoredInventory;
	for (int32 Index = 0; Index < Save->InventoryInstanceSnapshots.Num(); ++Index)
	{
		FItemInstance Instance;
		FString SnapshotText;
		FString ResolveError;
		if (!M5_046_ResolveInstanceSnapshot(Save->InventoryInstanceSnapshots[Index], ActiveManifest,
			Instance, SnapshotText, ResolveError))
		{
			OutError.Code = ResolveError.Contains(TEXT("definition_id"))
				? EProfileLoadError::UnknownItemDefinition
				: EProfileLoadError::BadInstanceSnapshot;
			OutError.Message = FString::Printf(
				TEXT("InventoryInstances[%d]: %s"), Index, *ResolveError);
			return false;
		}
		const EInventoryAddResult AddResult = RestoredInventory.TryAdd(Instance);
		if (AddResult != EInventoryAddResult::Added)
		{
			OutError.Code = AddResult == EInventoryAddResult::Duplicate
				? EProfileLoadError::DuplicateInstanceId
				: EProfileLoadError::InventoryOverfull;
			OutError.Message = FString::Printf(
				TEXT("InventoryInstances[%d]: restoring InstanceId '%s' returned result code %d; refusing to silently drop it"),
				Index, *Instance.InstanceId.ToString(), static_cast<int32>(AddResult));
			return false;
		}
	}

	TArray<FPendingReward> RestoredPending;
	for (int32 DraftIndex = 0; DraftIndex < Save->PendingRewards.Num(); ++DraftIndex)
	{
		const FPendingRewardSaveEntry& Entry = Save->PendingRewards[DraftIndex];
		FPendingReward Draft;
		Draft.SettlementId = Entry.SettlementId;
		Draft.XP = Entry.XP;
		for (int32 ItemIndex = 0; ItemIndex < Entry.ItemSnapshots.Num(); ++ItemIndex)
		{
			FItemInstance Item;
			FString SnapshotText;
			FString ResolveError;
			if (!M5_046_ResolveInstanceSnapshot(Entry.ItemSnapshots[ItemIndex], ActiveManifest,
				Item, SnapshotText, ResolveError))
			{
				OutError.Code = ResolveError.Contains(TEXT("definition_id"))
					? EProfileLoadError::UnknownItemDefinition
					: EProfileLoadError::BadInstanceSnapshot;
				OutError.Message = FString::Printf(
					TEXT("PendingRewards[%d].Items[%d]: %s"),
					DraftIndex, ItemIndex, *ResolveError);
				return false;
			}
			Draft.Items.Add(Item);
		}
		RestoredPending.Add(Draft);
	}

	TSet<uint64> RestoredApplied;
	for (const uint64 Id : Save->AppliedSettlementIds)
	{
		RestoredApplied.Add(Id);
	}

	TMap<EItemSlot, FGuid> RestoredEquipped;
	for (const FEquippedSlotSavePair& Pair : Save->EquippedIds)
	{
		RestoredEquipped.Add(static_cast<EItemSlot>(Pair.Slot), Pair.InstanceId);
	}

	// Commit: the snapshot carries identity/progress/base stats plus a copy of
	// the restored inventory; the other containers mirror the saved data.
	Restored.Inventory = RestoredInventory;
	OutSnapshot = MoveTemp(Restored);
	OutInventory = MoveTemp(RestoredInventory);
	OutPendingRewards = MoveTemp(RestoredPending);
	OutAppliedSettlementIds = MoveTemp(RestoredApplied);
	OutEquippedMap = MoveTemp(RestoredEquipped);

	// M5-046: hand the weapon state to the caller when asked - exactly what
	// the (possibly migrated) save carried, nothing derived on top.
	if (OutWeaponState)
	{
		OutWeaponState->WeaponMagazines = Save->WeaponMagazines;
		OutWeaponState->ReserveAmmoPools = Save->ReserveAmmoPools;
		OutWeaponState->bWeaponStateInitialized = Save->bWeaponStateInitialized;
		OutWeaponState->ConfigRevision = Save->ConfigRevision;
	}

	// M6-003: hand the per-room clear records to the caller when asked - the
	// (possibly migrated) save's rows verbatim (a v1/v2 input migrates to an
	// explicitly EMPTY set).
	if (OutRoomClears)
	{
		*OutRoomClears = Save->RoomClears;
	}

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return true;
}

UProfileSaveGame* FProfileSerializer::MigrateV1ToV2(const UProfileSaveGame& LegacySave,
	const FProfileItemManifest& ItemManifest,
	FProfileLoadError& OutError,
	const FString& MigrationConfigRevision,
	UObject* Outer)
{
	// The migration is EXPLICITLY v1 -> v2: anything else is a caller bug or
	// an unknown schema, and the original save stays untouched either way.
	if (LegacySave.SchemaVersion == UProfileSaveGame::CurrentSchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("the save already carries schema_version %d; there is nothing to migrate (the migration channel accepts the legacy schema_version %d only)"),
			UProfileSaveGame::CurrentSchemaVersion, UProfileSaveGame::LegacySchemaVersion);
		return nullptr;
	}
	if (LegacySave.SchemaVersion != UProfileSaveGame::LegacySchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d is not the legacy schema_version %d this migration accepts; refusing to load and refusing to modify or wipe the existing save file"),
			LegacySave.SchemaVersion, UProfileSaveGame::LegacySchemaVersion);
		return nullptr;
	}

	// The v1 payload must be a VALID v1 payload: the same field rules a load
	// applies (level range, finite settings, parseable snapshots, unique
	// instance ids, closed equipment slots, manifest-resolvable definitions).
	// A corrupt legacy save is refused here - never partially migrated.
	if (!Validate(&LegacySave, OutError, &ItemManifest))
	{
		return nullptr;
	}

	// Build the v2 successor. Every M3-013 base field is copied verbatim: the
	// migration preserves identity, progress, items, drafts and claims - no
	// re-forge, no re-roll, no silent deletion.
	UProfileSaveGame* Migrated = NewObject<UProfileSaveGame>(Outer ? Outer : GetTransientPackage());
	Migrated->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
	Migrated->CharacterId = LegacySave.CharacterId;
	Migrated->Level = LegacySave.Level;
	Migrated->XP = LegacySave.XP;
	Migrated->MasterVolume = LegacySave.MasterVolume;

	// Inventory instances: keep every non-aliased snapshot string byte-for-
	// byte; rewrite ONLY a mapped legacy id onto its current kind (the parse/
	// emit round trip carries InstanceId/RollSeed/Level/RolledStats value-
	// exactly, so the identity survives untouched). The resolved instances
	// also drive the weapon-state seeding below.
	TArray<FItemInstance> ResolvedInventory;
	for (const FString& SnapshotText : LegacySave.InventoryInstanceSnapshots)
	{
		FItemInstance Instance;
		FString ResolvedText;
		FString ResolveError;
		if (!M5_046_ResolveInstanceSnapshot(SnapshotText, &ItemManifest, Instance, ResolvedText, ResolveError))
		{
			// Defensive: Validate above already accepted every snapshot.
			OutError.Code = EProfileLoadError::BadInstanceSnapshot;
			OutError.Message = FString::Printf(TEXT("InventoryInstances: %s"), *ResolveError);
			return nullptr;
		}
		Migrated->InventoryInstanceSnapshots.Add(ResolvedText);
		ResolvedInventory.Add(Instance);
	}

	// Equipment references travel verbatim - the InstanceIds never changed,
	// so every binding keeps pointing at the same instance.
	Migrated->EquippedIds = LegacySave.EquippedIds;

	// Pending drafts: value fields verbatim, item snapshots through the same
	// alias treatment as the inventory (a legacy charm inside an unclaimed
	// draft must resolve at claim time too).
	for (const FPendingRewardSaveEntry& Entry : LegacySave.PendingRewards)
	{
		FPendingRewardSaveEntry MigratedEntry;
		MigratedEntry.SettlementId = Entry.SettlementId;
		MigratedEntry.XP = Entry.XP;
		for (const FString& SnapshotText : Entry.ItemSnapshots)
		{
			FItemInstance Item;
			FString ResolvedText;
			FString ResolveError;
			if (!M5_046_ResolveInstanceSnapshot(SnapshotText, &ItemManifest, Item, ResolvedText, ResolveError))
			{
				OutError.Code = EProfileLoadError::BadInstanceSnapshot;
				OutError.Message = FString::Printf(TEXT("PendingRewards[%llu].Items: %s"), Entry.SettlementId, *ResolveError);
				return nullptr;
			}
			MigratedEntry.ItemSnapshots.Add(ResolvedText);
		}
		Migrated->PendingRewards.Add(MigratedEntry);
	}

	// The claimed-set travels verbatim: claiming idempotency is preserved.
	Migrated->AppliedSettlementIds = LegacySave.AppliedSettlementIds;

	// The one-time weapon-state initialization: every inventory instance whose
	// (resolved) definition carries a manifest seed gets an explicit magazine
	// row - keyed by ITS InstanceId, seeded EMPTY (LoadedRounds 0, the
	// production idiom: mounting never mints ammunition). Melee definitions
	// carry no row and the shared reserve pools start absent (they provision
	// lazily and empty); bWeaponStateInitialized marks the domain settled so
	// no later path mistakes this state for "never initialized" and re-runs
	// an initial-ammo grant.
	for (const FItemInstance& Instance : ResolvedInventory)
	{
		if (const FProfileItemManifest::FWeaponAmmoSeed* Seed = ItemManifest.WeaponAmmoSeeds.Find(Instance.DefinitionId))
		{
			FWeaponMagazineSaveEntry Magazine;
			Magazine.InstanceId = Instance.InstanceId;
			Magazine.AmmoId = Seed->AmmoId;
			Magazine.Capacity = Seed->MagazineCapacity;
			Magazine.LoadedRounds = 0;
			Migrated->WeaponMagazines.Add(Magazine);
		}
	}
	Migrated->ReserveAmmoPools = LegacySave.ReserveAmmoPools; // empty in v1
	Migrated->bWeaponStateInitialized = true;
	Migrated->ConfigRevision = MigrationConfigRevision;

	// Defensive self-check: the migration's own output must validate as v2.
	FProfileLoadError SelfCheck;
	if (!Validate(Migrated, SelfCheck, &ItemManifest))
	{
		OutError.Code = SelfCheck.Code;
		OutError.Message = FString::Printf(TEXT("the migrated v2 save failed its own validation: %s"), *SelfCheck.Message);
		return nullptr;
	}

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return Migrated;
}

UProfileSaveGame* FProfileSerializer::MigrateV2ToV3(const UProfileSaveGame& LegacySave,
	FProfileLoadError& OutError,
	UObject* Outer)
{
	// The migration is EXPLICITLY v2 -> v3: anything else is a caller bug or
	// an unknown schema, and the original save stays untouched either way.
	if (LegacySave.SchemaVersion == UProfileSaveGame::CurrentSchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("the save already carries schema_version %d; there is nothing to migrate (this channel accepts schema_version %d only)"),
			UProfileSaveGame::CurrentSchemaVersion, UProfileSaveGame::SchemaVersion2);
		return nullptr;
	}
	if (LegacySave.SchemaVersion != UProfileSaveGame::SchemaVersion2)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d is not the schema_version %d this migration accepts; refusing to load and refusing to modify or wipe the existing save file"),
			LegacySave.SchemaVersion, UProfileSaveGame::SchemaVersion2);
		return nullptr;
	}

	// The v2 payload must be a VALID v2 payload: the same field rules a load
	// applies (level range, parseable snapshots, unique ids, weapon-state row
	// rules). The manifest gate stays with the caller's post-migration load
	// validation - v2 -> v3 performs no content rewriting. A corrupt legacy
	// save is refused here - never partially migrated.
	if (!Validate(&LegacySave, OutError))
	{
		return nullptr;
	}

	// v3 adds ONLY the RoomClears record set, so every v2 field copies
	// verbatim: no re-forge, no re-roll, no silent deletion. RoomClears seeds
	// explicitly EMPTY (a migrated save "has cleared no rooms yet" - the same
	// nothing-stays-implicit rule as the v1 weapon-state seeding).
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
	Migrated->RoomClears.Reset();

	// Defensive self-check: the migration's own output must validate as v3.
	FProfileLoadError SelfCheck;
	if (!Validate(Migrated, SelfCheck))
	{
		OutError.Code = SelfCheck.Code;
		OutError.Message = FString::Printf(TEXT("the migrated v3 save failed its own validation: %s"), *SelfCheck.Message);
		return nullptr;
	}

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return Migrated;
}

bool FProfileSerializer::IsWeaponStateCompatibleWithAmmoConfig(const UProfileSaveGame& Save,
	const TMap<FName, FAmmoType>& CurrentAmmoTypes,
	FProfileLoadError& OutError)
{
	// The revision strings are deliberately never compared: ConfigRevision is
	// provenance (interface contract section 7), and "the config changed" is
	// legal whenever the saved state still fits. The decision reads ONLY the
	// saved weapon state against the CURRENT ammo rows.

	// Per-instance magazines: the referenced kind must still exist and the
	// current magazine capacity must still hold the stored load - a shrunk
	// magazine must not silently spill rounds ("容量小于已装弹" refuses).
	for (int32 Index = 0; Index < Save.WeaponMagazines.Num(); ++Index)
	{
		const FWeaponMagazineSaveEntry& Magazine = Save.WeaponMagazines[Index];
		const FAmmoType* CurrentType = CurrentAmmoTypes.Find(Magazine.AmmoId);
		if (!CurrentType)
		{
			OutError.Code = EProfileLoadError::InvalidWeaponState;
			OutError.Message = FString::Printf(
				TEXT("WeaponMagazines[%d]: the saved state references ammo kind '%s' which the current config no longer defines; refusing to load instead of destroying the rounds (recorded revision '%s')"),
				Index, *Magazine.AmmoId.ToString(), *Save.ConfigRevision);
			return false;
		}
		if (CurrentType->MagazineSize < Magazine.LoadedRounds)
		{
			OutError.Code = EProfileLoadError::InvalidWeaponState;
			OutError.Message = FString::Printf(
				TEXT("WeaponMagazines[%d]: the current magazine capacity of '%s' is %d but the saved state holds %d loaded rounds; refusing to load instead of dropping them (recorded revision '%s')"),
				Index, *Magazine.AmmoId.ToString(), CurrentType->MagazineSize, Magazine.LoadedRounds, *Save.ConfigRevision);
			return false;
		}
	}

	// Shared reserve pools: the kind must still exist and the current cap
	// must still hold the stored stock (a pool the AmmoModel could not even
	// register is an unloadable state).
	for (int32 Index = 0; Index < Save.ReserveAmmoPools.Num(); ++Index)
	{
		const FAmmoReserveSaveEntry& Pool = Save.ReserveAmmoPools[Index];
		const FAmmoType* CurrentType = CurrentAmmoTypes.Find(Pool.AmmoId);
		if (!CurrentType)
		{
			OutError.Code = EProfileLoadError::InvalidWeaponState;
			OutError.Message = FString::Printf(
				TEXT("ReserveAmmoPools[%d]: the saved state references ammo kind '%s' which the current config no longer defines; refusing to load instead of destroying the reserve (recorded revision '%s')"),
				Index, *Pool.AmmoId.ToString(), *Save.ConfigRevision);
			return false;
		}
		if (CurrentType->MaxReserve < Pool.Rounds)
		{
			OutError.Code = EProfileLoadError::InvalidWeaponState;
			OutError.Message = FString::Printf(
				TEXT("ReserveAmmoPools[%d]: the current reserve cap of '%s' is %d but the saved state holds %d rounds; refusing to load instead of destroying them (recorded revision '%s')"),
				Index, *Pool.AmmoId.ToString(), CurrentType->MaxReserve, Pool.Rounds, *Save.ConfigRevision);
			return false;
		}
	}

	// An empty weapon state (every v1 input, any content-less profile) is
	// trivially compatible: there is nothing the config change could break.
	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return true;
}
