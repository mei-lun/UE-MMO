#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "ItemDefinition.h"
#include "InventoryModel.h"

/**
 * Result of FEquipmentModel::Equip (interface contract section 8, M3-004).
 * Every rejection reason is a distinct value and a rejected Equip never
 * modifies the slot mapping nor the passed inventory.
 */
enum class EEquipmentEquipResult : uint8
{
	/** The slot now maps to InstanceId (fresh equip or same-slot replacement). */
	Equipped,

	/**
	 * The slot already maps to exactly this InstanceId; idempotent no-op. No
	 * second mapping is created (no stat stacking) and the inventory is not
	 * touched again (no double deduction - equipping never deducts anyway).
	 */
	AlreadyEquipped,

	/**
	 * No instance with that InstanceId is stored in the passed inventory
	 * (this also covers the all-zero guid, which can never be stored);
	 * nothing changed. Equipment only ever references inventory instances.
	 */
	NotInInventory,

	/**
	 * The instance's definition does not confirm the requested slot: the
	 * DefinitionId is unknown to the bound catalog, or it resolves to a
	 * different EItemSlot; nothing changed. An unknown definition is folded
	 * here on purpose: from the slot constraint's point of view both cases
	 * mean "the slot match could not be confirmed" (no silent fallback).
	 */
	SlotMismatch,

	/** The requested Slot is not one of the three closed enum values; nothing changed. */
	InvalidSlot,

	/**
	 * No definition catalog is bound (SetDefinitionCatalog not called), so the
	 * slot-match check cannot run; nothing changed. The check is never
	 * bypassed silently - equip without a definition source is an error.
	 */
	MissingDefinitions
};

/**
 * Result of FEquipmentModel::Unequip. A rejected Unequip is a pure no-op.
 */
enum class EEquipmentUnequipResult : uint8
{
	/** The slot held a mapping and it was cleared. */
	Unequipped,

	/** The slot was already empty; idempotent no-op. */
	NotEquipped,

	/** The requested Slot is not one of the three closed enum values. */
	InvalidSlot
};

/**
 * Pure-logic equipment slot mapping (interface contract section 8, M3-004):
 * three closed slots (Weapon/Armor/Accessory), each holding at most one
 * inventory instance id. No UObject, no World, no ticking. The mapping only
 * ever REFERENCES instances that live in an FInventoryModel - equipping never
 * adds, copies or removes items, and unequipping just clears the id mapping,
 * so the item "stays in the inventory" by construction.
 *
 * Coordination with the inventory (why no include cycle): this header
 * includes InventoryModel.h, and the inventory never includes this header.
 * Instead the inventory holds an opaque equip-guard predicate
 * (TFunction<bool(const FGuid&)>); AttachToInventory registers a callback
 * into FInventoryModel::SetEquippedPredicate that answers "is this instance
 * currently equipped" via IsInstanceEquipped. From then on
 * FInventoryModel::Remove refuses (EInventoryRemoveResult::ItemEquipped) to
 * delete an equipped instance, so a slot mapping can never dangle behind the
 * equipment's back. The wiring is per-object: FInventoryModel drops guards on
 * copy/assignment (they are wiring, not data), so detached inventory copies
 * (e.g. profile snapshots) never call back here; re-attach after replacing
 * the inventory. This model must outlive the registration.
 *
 * Slot-match checking needs definition data (an FItemInstance carries only a
 * DefinitionId), so Equip consults the read-only FItemDefinitionCatalog bound
 * via SetDefinitionCatalog (non-owning pointer). Without a bound catalog
 * Equip fails with MissingDefinitions instead of skipping the check.
 *
 * Not this card's business (kept out on purpose): stat summation belongs to
 * FStatCalculator (M3-005) - this model never adds or subtracts stats, it
 * only maps slots to instance ids.
 */
struct FEquipmentModel
{
	/**
	 * Binds the read-only definition source used to confirm slot matches.
	 * Non-owning: the catalog must outlive the model's use. Passing nullptr
	 * unbinds; Equip then fails with MissingDefinitions (never silently).
	 */
	void SetDefinitionCatalog(const FItemDefinitionCatalog* Catalog);

	/**
	 * Registers this model as the inventory's equip guard: from now on
	 * FInventoryModel::Remove returns ItemEquipped (no-op) for every instance
	 * this model currently maps in some slot. The registration captures this
	 * (non-owning) - attach AFTER the model reaches its final storage place
	 * and never copy an attached model. See the struct comment for lifetime.
	 */
	void AttachToInventory(FInventoryModel& Inventory);

	/**
	 * Equips the inventory instance with the given id into the given slot.
	 * Check order: InvalidSlot, MissingDefinitions, NotInInventory,
	 * SlotMismatch, then the slot write. Equipping an instance into its
	 * matching empty slot stores the mapping (Equipped); equipping into a
	 * slot that already holds the SAME id is the idempotent AlreadyEquipped
	 * no-op; equipping another valid instance into an occupied slot replaces
	 * only the id mapping (Equipped) - the previously equipped item stays in
	 * the inventory untouched, nothing is added or deleted anywhere. A
	 * rejected Equip never modifies the mapping nor the inventory.
	 */
	EEquipmentEquipResult Equip(EItemSlot Slot, const FGuid& InstanceId, const FInventoryModel& Inventory);

	/**
	 * Clears the given slot's mapping. The item itself was never moved: it
	 * simply remains stored in the inventory and can be equipped again.
	 * Unequipping an empty slot is the idempotent NotEquipped no-op.
	 */
	EEquipmentUnequipResult Unequip(EItemSlot Slot);

	/**
	 * Read-only mapping lookup; nullptr when the slot is empty (and for
	 * out-of-enum slot values), mirroring FInventoryModel::GetByIndex.
	 */
	const FGuid* GetEquippedId(EItemSlot Slot) const;

	/** True when the slot currently holds a mapping (false for out-of-enum slots). */
	bool IsSlotEquipped(EItemSlot Slot) const;

	/**
	 * True when some slot maps to the given InstanceId. This is the guard
	 * answer behind FInventoryModel::Remove's ItemEquipped result.
	 */
	bool IsInstanceEquipped(const FGuid& InstanceId) const;

	/** Number of currently occupied slots (0..3); each slot holds at most one id. */
	int32 NumEquippedSlots() const;

	/** Clears every slot mapping (new-profile lifecycle); catalog binding survives. */
	void Reset();

private:
	/** Slot -> inventory instance id; at most one entry per closed slot value. */
	TMap<EItemSlot, FGuid> SlotToInstance;

	/** Non-owning definition source for slot-match checks; nullptr until bound. */
	const FItemDefinitionCatalog* DefinitionCatalog = nullptr;
};
