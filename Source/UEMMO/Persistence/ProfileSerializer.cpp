// M3-013: implementation of the pure profile save serializer. No World, no
// subsystem, no file I/O: ToSaveGame maps trusted in-memory profile state into
// a fresh UProfileSaveGame; Validate accepts/rejects a save against the
// schema_version=1 rules; FromSaveGame restores into caller-owned containers
// only after validation passed, and never touches any Out* parameter on the
// error path (a rejected load can never surface as a half-restored profile or
// a silently dropped item).

#include "ProfileSerializer.h"

#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"

#include "UObject/UObjectGlobals.h"

UProfileSaveGame* FProfileSerializer::ToSaveGame(const FProfileSnapshot& Snapshot,
	const FInventoryModel& Inventory,
	const TArray<FPendingReward>& PendingRewards,
	const TSet<uint64>& AppliedSettlementIds,
	const TMap<EItemSlot, FGuid>& EquippedMap,
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

	return Save;
}

bool FProfileSerializer::Validate(const UProfileSaveGame* Save, FProfileLoadError& OutError)
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
	if (Save->SchemaVersion > UProfileSaveGame::CurrentSchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d was written by a newer game build (this build understands schema_version %d); refusing to load and refusing to modify or wipe the existing save file"),
			Save->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
		return false;
	}
	if (Save->SchemaVersion < UProfileSaveGame::CurrentSchemaVersion)
	{
		OutError.Code = EProfileLoadError::UnsupportedSchema;
		OutError.Message = FString::Printf(
			TEXT("schema_version %d is not a schema this build knows (expected %d); refusing to load and refusing to modify or wipe the existing save file"),
			Save->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
		return false;
	}

	// Progression: the closed 1..MaxLevel range (Level 0 means "no profile",
	// which is never a legal on-disk state).
	if (Save->Level < 1 || Save->Level > UProfileSubsystem::MaxLevel)
	{
		OutError.Code = EProfileLoadError::InvalidLevel;
		OutError.Message = FString::Printf(
			TEXT("Level %d is outside the closed range 1..%d"),
			Save->Level, UProfileSubsystem::MaxLevel);
		return false;
	}

	// The settings placeholder float must be finite.
	if (!FMath::IsFinite(Save->MasterVolume))
	{
		OutError.Code = EProfileLoadError::NonFiniteSetting;
		OutError.Message = TEXT("MasterVolume must be a finite number");
		return false;
	}

	// Capacity: a save claiming more items than the inventory can hold is
	// corrupt; the overflow must be refused, never silently dropped.
	if (Save->InventoryInstanceSnapshots.Num() > FInventoryModel::Capacity)
	{
		OutError.Code = EProfileLoadError::InventoryOverfull;
		OutError.Message = FString::Printf(
			TEXT("InventoryInstances carries %d snapshot strings, more than the capacity %d; refusing to silently drop the overflow"),
			Save->InventoryInstanceSnapshots.Num(), FInventoryModel::Capacity);
		return false;
	}

	// Parse every item snapshot and enforce GLOBAL instance-id uniqueness
	// across the inventory AND all pending drafts. FromStringSnapshot itself
	// rejects invalid guids, duplicate/unknown fields and non-finite (NaN/Inf)
	// stats with a field-naming error, which is surfaced verbatim here.
	// InventoryIds (inventory only) is kept separate from the global set
	// because equipment may reference inventory instances only.
	TSet<FGuid> InventoryIds;
	TSet<FGuid> AllInstanceIds;
	for (int32 Index = 0; Index < Save->InventoryInstanceSnapshots.Num(); ++Index)
	{
		FItemInstance Instance;
		FString ParseError;
		if (!Instance.FromStringSnapshot(Save->InventoryInstanceSnapshots[Index], &ParseError))
		{
			OutError.Code = EProfileLoadError::BadInstanceSnapshot;
			OutError.Message = FString::Printf(
				TEXT("InventoryInstances[%d]: %s"), Index, *ParseError);
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

	for (int32 DraftIndex = 0; DraftIndex < Save->PendingRewards.Num(); ++DraftIndex)
	{
		const FPendingRewardSaveEntry& Entry = Save->PendingRewards[DraftIndex];
		for (int32 ItemIndex = 0; ItemIndex < Entry.ItemSnapshots.Num(); ++ItemIndex)
		{
			FItemInstance Item;
			FString ParseError;
			if (!Item.FromStringSnapshot(Entry.ItemSnapshots[ItemIndex], &ParseError))
			{
				OutError.Code = EProfileLoadError::BadInstanceSnapshot;
				OutError.Message = FString::Printf(
					TEXT("PendingRewards[%d].Items[%d]: %s"),
					DraftIndex, ItemIndex, *ParseError);
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
	for (int32 Index = 0; Index < Save->EquippedIds.Num(); ++Index)
	{
		const FEquippedSlotSavePair& Pair = Save->EquippedIds[Index];
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

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return true;
}

bool FProfileSerializer::FromSaveGame(const UProfileSaveGame* Save,
	FProfileSnapshot& OutSnapshot,
	FInventoryModel& OutInventory,
	TArray<FPendingReward>& OutPendingRewards,
	TSet<uint64>& OutAppliedSettlementIds,
	TMap<EItemSlot, FGuid>& OutEquippedMap,
	FProfileLoadError& OutError)
{
	// Single validation gate; on failure no Out* parameter is written.
	if (!Validate(Save, OutError))
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
		FString ParseError;
		if (!Instance.FromStringSnapshot(Save->InventoryInstanceSnapshots[Index], &ParseError))
		{
			OutError.Code = EProfileLoadError::BadInstanceSnapshot;
			OutError.Message = FString::Printf(
				TEXT("InventoryInstances[%d]: %s"), Index, *ParseError);
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
			FString ParseError;
			if (!Item.FromStringSnapshot(Entry.ItemSnapshots[ItemIndex], &ParseError))
			{
				OutError.Code = EProfileLoadError::BadInstanceSnapshot;
				OutError.Message = FString::Printf(
					TEXT("PendingRewards[%d].Items[%d]: %s"),
					DraftIndex, ItemIndex, *ParseError);
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

	OutError.Code = EProfileLoadError::None;
	OutError.Message.Reset();
	return true;
}
