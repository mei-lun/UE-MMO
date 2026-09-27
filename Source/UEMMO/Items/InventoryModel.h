#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Templates/Function.h"

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
 * item untouched; an ItemEquipped removal also leaves every stored item
 * untouched (the instance is referenced by an equipment slot and must be
 * unequipped first, M3-004).
 */
enum class EInventoryRemoveResult : uint8
{
	/** The one instance with the requested InstanceId was removed. */
	Removed,

	/** No instance with that InstanceId exists; nothing changed. */
	NotFound,

	/**
	 * The instance exists but is currently referenced by an equipment slot
	 * (the equip guard predicate returned true, M3-004); removal refused and
	 * nothing changed. The caller must unequip the slot before removing.
	 */
	ItemEquipped
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

	/** A fresh, empty, unguarded inventory. */
	FInventoryModel() = default;

	/**
	 * Copy construction copies the stored items but deliberately drops the
	 * equip guard (see SetEquippedPredicate): the copy is detached data and
	 * must never keep querying the original wiring's equipment model.
	 */
	FInventoryModel(const FInventoryModel& Other);

	/** Same rule as the copy constructor: items are copied, the guard is not. */
	FInventoryModel& operator=(const FInventoryModel& Other);

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
	 * item when no entry carries the id. When an equip guard is installed
	 * (SetEquippedPredicate) and the requested instance is currently
	 * referenced by an equipment slot, returns ItemEquipped as a pure no-op:
	 * an equipped item must be unequipped before it can be removed.
	 */
	EInventoryRemoveResult Remove(const FGuid& InstanceId);

	/**
	 * Equip-guard predicate type (M3-004): returns true when the given
	 * InstanceId is currently referenced by an equipment slot. Stored as a
	 * TFunction instead of a direct FEquipmentModel reference so the inventory
	 * never needs to know the equipment type (no include cycle, no compile
	 * coupling); the wiring is installed by FEquipmentModel::AttachToInventory.
	 */
	using FEquippedPredicate = TFunction<bool(const FGuid&)>;

	/**
	 * Installs (replaces, or clears with an empty TFunction) the equip guard
	 * consulted by Remove. This is operational wiring, not container data: the
	 * predicate is deliberately NOT carried along by copy construction or copy
	 * assignment, so a detached copy (e.g. a profile snapshot's inventory copy)
	 * always starts unguarded and can never call back into a foreign or
	 * outlived equipment model. Re-attach after copying or wholesale
	 * reassigning an inventory whose guard should stay live.
	 */
	void SetEquippedPredicate(FEquippedPredicate Predicate);

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

	/** Optional equip guard (M3-004); empty on every fresh or copied model. */
	FEquippedPredicate EquippedPredicate;
};
