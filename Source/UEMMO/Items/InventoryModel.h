#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "ItemInstance.h"

/**
 * Result of FInventoryModel::TryAdd (interface contract section 8). Every
 * rejection reason is a distinct value, and a rejected TryAdd never modifies
 * the container: the caller keeps full ownership of the instance it passed in.
 */
enum class EInventoryAddResult : uint8
{
	/** The instance was stored; the model now owns a copy of it. */
	Added,

	/** An instance with the same InstanceId is already stored; nothing changed. */
	Duplicate,

	/** The inventory already holds Capacity items; nothing changed. */
	InventoryFull,

	/** The instance's InstanceId is not a valid FGuid (e.g. all-zero); nothing changed. */
	InvalidInstance
};

/**
 * Result of FInventoryModel::Remove. A NotFound removal leaves every stored
 * item untouched.
 */
enum class EInventoryRemoveResult : uint8
{
	/** The one instance with the requested InstanceId was removed. */
	Removed,

	/** No instance with that InstanceId exists; nothing changed. */
	NotFound
};

/**
 * Pure-logic 30-slot inventory (interface contract section 8). The business
 * identity of a stored entry is the FItemInstance's persistent InstanceId:
 * duplicates are rejected, a full inventory refuses further additions without
 * dropping anything, and every mutation returns an explicit result code.
 * No UObject, no World, no ticking: the model is plain data plus functions, so
 * the whole behavior stays testable as pure logic (and later becomes the
 * backing store of the profile subsystem without any engine coupling).
 *
 * Slot order is insertion order and stays stable across removals: after a
 * Remove the later entries shift up by one, nothing swaps or reorders.
 *
 * TryAdd check order is InvalidInstance, then Duplicate, then InventoryFull:
 * an identity problem always outranks a space problem because an invalid or
 * already-owned instance can never be stored, full or not.
 */
struct FInventoryModel
{
	/** Fixed maximum number of stored instances (design constant). */
	static constexpr int32 Capacity = 30;

	/**
	 * Adds one copy of Instance, keyed by its InstanceId. Rejects an invalid
	 * InstanceId (InvalidInstance), an id that is already stored (Duplicate)
	 * and a full inventory (InventoryFull); in all three cases the container
	 * is left unchanged and the caller keeps the instance. Only Added stores
	 * a copy (appended after the currently stored items).
	 */
	EInventoryAddResult TryAdd(const FItemInstance& Instance);

	/**
	 * Removes the one instance whose InstanceId matches. Returns Removed after
	 * dropping exactly that entry, or NotFound without touching any other
	 * item when no entry carries the id.
	 */
	EInventoryRemoveResult Remove(const FGuid& InstanceId);

	/** Number of currently stored instances (0..Capacity). */
	int32 Count() const;

	/** True when an instance with the given InstanceId is currently stored. */
	bool Contains(const FGuid& InstanceId) const;

	/**
	 * Read-only slot access by insertion index (0..Count()-1). An out-of-range
	 * index returns nullptr instead of asserting, so a desynced count can
	 * never crash a caller that only queries.
	 */
	const FItemInstance* GetByIndex(int32 Index) const;

	/** Read-only view of all stored instances in insertion order. */
	const TArray<FItemInstance>& GetAll() const;

private:
	/** Stored instances in insertion order; never holds duplicate InstanceIds. */
	TArray<FItemInstance> Slots;
};
