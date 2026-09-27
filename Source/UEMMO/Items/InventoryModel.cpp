// M3-002: 30-slot inventory model, real implementation (interface contract
// section 8). TryAdd keys entries by the persistent InstanceId, rejects
// duplicates and full inventories without modifying the container, and Remove
// drops exactly one entry or reports NotFound as a no-op. Slot order stays in
// insertion order across removals (later entries shift up, nothing swaps).
#include "InventoryModel.h"

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
