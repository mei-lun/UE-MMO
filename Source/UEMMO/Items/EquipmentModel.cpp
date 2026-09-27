// M3-004: equipment slot mapping, real implementation (interface contract
// section 8). Equip writes the slot -> inventory-instance-id mapping only
// after the closed-slot value, the bound definition catalog, the inventory
// membership and the definition's own slot all confirm; a same-slot
// replacement rewrites just the id (the replaced item stays stored), and a
// repeated same-id equip is the AlreadyEquipped no-op. Unequip clears the
// mapping without touching any item. The model never adds, copies, removes
// or modifies instances, and never sums stats (FStatCalculator, M3-005).
#include "EquipmentModel.h"

void FEquipmentModel::SetDefinitionCatalog(const FItemDefinitionCatalog* Catalog)
{
	// Non-owning binding; nullptr is a legal "unbind" that makes every later
	// Equip fail with MissingDefinitions instead of skipping the check.
	DefinitionCatalog = Catalog;
}

void FEquipmentModel::AttachToInventory(FInventoryModel& Inventory)
{
	// Register the equip guard as an opaque predicate: the inventory stays
	// decoupled from this type (no include cycle) and Remove answers
	// ItemEquipped for any instance this model maps in some slot. Captures
	// this (non-owning); the model must outlive the registration, and
	// FInventoryModel drops guards on copy/assignment so detached copies
	// never call back here.
	Inventory.SetEquippedPredicate(
		[this](const FGuid& InstanceId) { return IsInstanceEquipped(InstanceId); });
}

EEquipmentEquipResult FEquipmentModel::Equip(EItemSlot Slot, const FGuid& InstanceId, const FInventoryModel& Inventory)
{
	// 1) The closed slot set is checked first: an out-of-enum value must not
	//    fall through into the mapping or bypass any later check.
	if (!IsValidItemSlot(Slot))
	{
		return EEquipmentEquipResult::InvalidSlot;
	}
	// 2) The slot-match check needs definition data; without a bound catalog
	//    it cannot run, and it is never bypassed silently.
	if (!DefinitionCatalog)
	{
		return EEquipmentEquipResult::MissingDefinitions;
	}
	// 3) Equipment only ever references inventory instances: resolve the id
	//    in the passed inventory (the all-zero guid is never stored, so it
	//    fails here as NotInInventory too).
	const FItemInstance* Stored = nullptr;
	for (const FItemInstance& Candidate : Inventory.GetAll())
	{
		if (Candidate.InstanceId == InstanceId)
		{
			Stored = &Candidate;
			break;
		}
	}
	if (!Stored)
	{
		return EEquipmentEquipResult::NotInInventory;
	}
	// 4) The instance's definition must confirm the requested slot. An
	//    unknown DefinitionId cannot confirm any slot and fails the same way
	//    (no silent fallback).
	const FItemDefinition* Definition = DefinitionCatalog->Find(Stored->DefinitionId);
	if (!Definition || Definition->Slot != Slot)
	{
		return EEquipmentEquipResult::SlotMismatch;
	}
	// 5) All constraints confirmed: write the mapping. A same-slot re-equip
	//    of the identical id is the idempotent AlreadyEquipped no-op (no
	//    second entry, no stat stacking, no deduction); a different valid id
	//    replaces the mapping only - the replaced item simply stays stored.
	if (FGuid* Existing = SlotToInstance.Find(Slot))
	{
		if (*Existing == InstanceId)
		{
			return EEquipmentEquipResult::AlreadyEquipped;
		}
		*Existing = InstanceId;
		return EEquipmentEquipResult::Equipped;
	}
	SlotToInstance.Add(Slot, InstanceId);
	return EEquipmentEquipResult::Equipped;
}

EEquipmentUnequipResult FEquipmentModel::Unequip(EItemSlot Slot)
{
	if (!IsValidItemSlot(Slot))
	{
		return EEquipmentUnequipResult::InvalidSlot;
	}
	// Clearing the mapping moves nothing: the item remains stored in the
	// inventory and can be equipped again. An empty slot is a no-op.
	return SlotToInstance.Remove(Slot) > 0
		? EEquipmentUnequipResult::Unequipped
		: EEquipmentUnequipResult::NotEquipped;
}

const FGuid* FEquipmentModel::GetEquippedId(EItemSlot Slot) const
{
	// nullptr for an empty slot (and for out-of-enum values, which can never
	// be mapped), mirroring FInventoryModel::GetByIndex.
	return SlotToInstance.Find(Slot);
}

bool FEquipmentModel::IsSlotEquipped(EItemSlot Slot) const
{
	return SlotToInstance.Contains(Slot);
}

bool FEquipmentModel::IsInstanceEquipped(const FGuid& InstanceId) const
{
	for (const TPair<EItemSlot, FGuid>& Pair : SlotToInstance)
	{
		if (Pair.Value == InstanceId)
		{
			return true;
		}
	}
	return false;
}

int32 FEquipmentModel::NumEquippedSlots() const
{
	return SlotToInstance.Num();
}

void FEquipmentModel::Reset()
{
	// Clear the mappings only: the catalog binding is configuration, not
	// equipment state, so it survives (matches the inventory lifecycle where
	// a new game empties the slots but keeps the definition data source).
	SlotToInstance.Reset();
}
