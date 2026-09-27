// M3-002: 30-slot inventory model, real implementation (interface contract
// section 8). TryAdd keys entries by the persistent InstanceId, rejects
// duplicates and full inventories without modifying the container, and Remove
// drops exactly one entry or reports NotFound as a no-op. Slot order stays in
// insertion order across removals (later entries shift up, nothing swaps).
// M3-004: Remove additionally consults an optional equip guard predicate and
// reports ItemEquipped (pure no-op) for instances an equipment model still
// references; the guard is wiring and is never carried across copies.
#include "InventoryModel.h"

// M3-004: copies copy the items, never the guard predicate. The guard is
// per-object wiring that captures external state (the equipment model), so a
// detached copy must start unguarded or a stale capture could fire from a
// snapshot-like copy. See SetEquippedPredicate in InventoryModel.h.
FInventoryModel::FInventoryModel(const FInventoryModel& Other)
	: Slots(Other.Slots)
{
}

FInventoryModel& FInventoryModel::operator=(const FInventoryModel& Other)
{
	if (this != &Other)
	{
		Slots = Other.Slots;
		EquippedPredicate = nullptr;
	}
	return *this;
}

void FInventoryModel::SetEquippedPredicate(FEquippedPredicate Predicate)
{
	EquippedPredicate = MoveTemp(Predicate);
}

EInventoryAddResult FInventoryModel::TryAdd(const FItemInstance& Instance)
{
	// The all-zero (or otherwise invalid) guid is never a legal business
	// identity; such an instance can never be stored, full or not.
	if (!Instance.InstanceId.IsValid())
	{
		return EInventoryAddResult::InvalidInstance;
	}
	// One instance id, one slot: a second add of the same identity is a
	// Duplicate, never an overwrite, so the first copy's payload survives.
	if (Contains(Instance.InstanceId))
	{
		return EInventoryAddResult::Duplicate;
	}
	// Full and the caller wants one more distinct instance: refuse without
	// touching the container so the caller keeps its item (no silent drop).
	if (Slots.Num() >= Capacity)
	{
		return EInventoryAddResult::InventoryFull;
	}
	Slots.Add(Instance);
	return EInventoryAddResult::Added;
}

EInventoryRemoveResult FInventoryModel::Remove(const FGuid& InstanceId)
{
	const int32 Index = Slots.IndexOfByPredicate(
		[&InstanceId](const FItemInstance& Slot) { return Slot.InstanceId == InstanceId; });
	if (Index == INDEX_NONE)
	{
		// Unknown (or all-zero) id: pure no-op, every stored item untouched.
		return EInventoryRemoveResult::NotFound;
	}
	// M3-004 equip guard: an instance referenced by an equipment slot must not
	// be deleted behind the equipment's back. Pure no-op; the caller has to
	// unequip the slot first, then retry the removal.
	if (EquippedPredicate && EquippedPredicate(InstanceId))
	{
		return EInventoryRemoveResult::ItemEquipped;
	}
	// RemoveAt keeps the relative order of the remaining entries.
	Slots.RemoveAt(Index);
	return EInventoryRemoveResult::Removed;
}

int32 FInventoryModel::Count() const
{
	return Slots.Num();
}

bool FInventoryModel::Contains(const FGuid& InstanceId) const
{
	return Slots.ContainsByPredicate(
		[&InstanceId](const FItemInstance& Slot) { return Slot.InstanceId == InstanceId; });
}

const FItemInstance* FInventoryModel::GetByIndex(int32 Index) const
{
	if (Index < 0 || Index >= Slots.Num())
	{
		return nullptr;
	}
	return &Slots[Index];
}

const TArray<FItemInstance>& FInventoryModel::GetAll() const
{
	return Slots;
}
