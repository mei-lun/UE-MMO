// M3-004: equipment slot constraints and equip/unequip (interface contract
// section 8). Pins the equip rules (instance must be stored in the passed
// inventory and its definition must confirm the requested slot), the
// same-slot replacement (id mapping only, both items stay stored, exactly one
// active item), the duplicate-equip idempotency (no second mapping, no
// deduction), the inventory coordination (Remove of an equipped instance
// returns ItemEquipped as a pure no-op), the unequip lifecycle (clear slot,
// item stays stored, re-equip works) and the untouched instance payloads.
// Pure checks: this file only builds plain structs and calls plain functions;
// it never touches UE assets, worlds or wall clocks.
//
// Stub-failure note: the red stub of EquipmentModel.cpp answers MissingDefinitions
// for every Equip, NotEquipped for every Unequip and reports an always-empty
// mapping, so each of the eight tests below fails at least one result-code or
// mapping assertion (only the no-catalog assertion inside
// EquipRejectsInvalidSlotAndMissingCatalog holds under the stub; the test
// still fails on its InvalidSlot assertion).

#include "Misc/AutomationTest.h"

#include "../Items/EquipmentModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_004
{
	/**
	 * Builds one valid item definition with a distinct stat shape, so the
	 * payload test can tell instances apart field by field.
	 */
	static FItemDefinition MakeM3_004Definition(const TCHAR* DefinitionName, EItemSlot Slot,
		float Attack, float Defense, float MaxHP)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = FName(DefinitionName);
		Definition.DisplayName = FString(DefinitionName);
		Definition.Slot = Slot;
		Definition.BaseStats.Attack = Attack;
		Definition.BaseStats.Defense = Defense;
		Definition.BaseStats.MaxHP = MaxHP;
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	/** The three-slot training catalog: one definition per closed slot value. */
	static FItemDefinitionCatalog MakeM3_004Catalog()
	{
		FItemDefinitionCatalog Catalog;
		Catalog.AddDefinition(MakeM3_004Definition(TEXT("weapon_training"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f), nullptr);
		Catalog.AddDefinition(MakeM3_004Definition(TEXT("armor_training"), EItemSlot::Armor, 0.0f, 3.0f, 0.0f), nullptr);
		Catalog.AddDefinition(MakeM3_004Definition(TEXT("charm_training"), EItemSlot::Accessory, 0.0f, 0.0f, 20.0f), nullptr);
		return Catalog;
	}

	/**
	 * Builds a valid instance with a deterministic FGuid derived from Seed (no
	 * NewGuid randomness) and caller-chosen stats, so payload comparisons are
	 * exact and reproducible.
	 */
	static FItemInstance MakeM3_004Instance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP, int32 Level)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(Seed, 0xBEEFu, Seed + 11u, Seed * 5u + 3u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(Seed);
		Instance.RolledStats.Attack = Attack;
		Instance.RolledStats.Defense = Defense;
		Instance.RolledStats.MaxHP = MaxHP;
		Instance.Level = Level;
		return Instance;
	}

	/**
	 * Setup helper: adds one instance and reports a concrete error naming the
	 * entry when TryAdd does not return Added. Tests bail out (early return)
	 * when the fixtures cannot be created.
	 */
	static bool AddM3_004OrReport(FAutomationTestBase& Test, FInventoryModel& Model,
		const FItemInstance& Instance, const TCHAR* What)
	{
		const EInventoryAddResult Result = Model.TryAdd(Instance);
		if (Result != EInventoryAddResult::Added)
		{
			Test.AddError(FString::Printf(TEXT("setup: TryAdd(%s) returned result code %d instead of Added"),
				What, static_cast<int32>(Result)));
			return false;
		}
		return true;
	}

	/** Linear lookup by InstanceId (the inventory exposes no direct by-id getter). */
	static const FItemInstance* FindM3_004ById(const FInventoryModel& Model, const FGuid& InstanceId)
	{
		for (const FItemInstance& Stored : Model.GetAll())
		{
			if (Stored.InstanceId == InstanceId)
			{
				return &Stored;
			}
		}
		return nullptr;
	}

	/** Exact payload equality: identity, definition, seed, level and all three stats. */
	static bool M3_004SamePayload(const FItemInstance& A, const FItemInstance& B)
	{
		return A.InstanceId == B.InstanceId
			&& A.DefinitionId == B.DefinitionId
			&& A.RollSeed == B.RollSeed
			&& A.Level == B.Level
			&& A.RolledStats.Attack == B.RolledStats.Attack
			&& A.RolledStats.Defense == B.RolledStats.Defense
			&& A.RolledStats.MaxHP == B.RolledStats.MaxHP;
	}
}

using namespace UE::UEMMO::Tasks::M3_004;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004EquipUnknownInstanceFailsWithoutMapping,
	"UEMMO.Tasks.M3_004.EquipUnknownInstanceFailsWithoutMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004EquipUnknownInstanceFailsWithoutMapping::RunTest(const FString& Parameters)
{
	// Equipping an id that is not stored in the passed inventory must fail
	// explicitly and leave both the mapping and the inventory untouched. The
	// all-zero guid can never be stored, so it fails the same way.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_004Instance(0x11u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	if (!AddM3_004OrReport(*this, Inventory, Weapon, TEXT("Weapon")))
	{
		return true;
	}

	const FGuid Stranger(0xDEADu, 0xBEEFu, 0xCAFEu, 0xF00Du);
	TestTrue(TEXT("equipping an id that was never added returns NotInInventory"),
		Equipment.Equip(EItemSlot::Weapon, Stranger, Inventory) == EEquipmentEquipResult::NotInInventory);
	TestTrue(TEXT("equipping the all-zero guid returns NotInInventory"),
		Equipment.Equip(EItemSlot::Weapon, FGuid(), Inventory) == EEquipmentEquipResult::NotInInventory);

	TestEqual(TEXT("no slot mapping was created"), Equipment.NumEquippedSlots(), 0);
	TestTrue(TEXT("the weapon slot is still empty"), Equipment.GetEquippedId(EItemSlot::Weapon) == nullptr);
	TestTrue(TEXT("no slot reports equipped"),
		!Equipment.IsSlotEquipped(EItemSlot::Weapon) && !Equipment.IsSlotEquipped(EItemSlot::Armor) &&
		!Equipment.IsSlotEquipped(EItemSlot::Accessory));
	TestEqual(TEXT("the failed equips did not change the inventory size"), Inventory.Count(), 1);
	TestTrue(TEXT("the stored weapon survived the failed equips"), Inventory.Contains(Weapon.InstanceId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004EquipSlotMismatchFails,
	"UEMMO.Tasks.M3_004.EquipSlotMismatchFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004EquipSlotMismatchFails::RunTest(const FString& Parameters)
{
	// An instance may only enter the slot its definition declares: weapon ->
	// Weapon, armor -> Armor, charm -> Accessory. Cross-slot equips and an
	// unknown DefinitionId (unresolvable in the bound catalog) must all fail
	// with SlotMismatch and store nothing.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_004Instance(0x21u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance Armor = MakeM3_004Instance(0x22u, TEXT("armor_training"), 0.0f, 3.0f, 0.0f, 1);
	const FItemInstance Charm = MakeM3_004Instance(0x23u, TEXT("charm_training"), 0.0f, 0.0f, 20.0f, 1);
	// The inventory stores instances without validating definitions, so an
	// instance whose DefinitionId the catalog does not know can exist there;
	// the equipment model must still refuse to equip it.
	const FItemInstance Ghost = MakeM3_004Instance(0x24u, TEXT("ghost_item"), 9.0f, 9.0f, 9.0f, 1);
	if (!AddM3_004OrReport(*this, Inventory, Weapon, TEXT("Weapon")) ||
		!AddM3_004OrReport(*this, Inventory, Armor, TEXT("Armor")) ||
		!AddM3_004OrReport(*this, Inventory, Charm, TEXT("Charm")) ||
		!AddM3_004OrReport(*this, Inventory, Ghost, TEXT("Ghost")))
	{
		return true;
	}

	TestTrue(TEXT("a weapon instance cannot enter the armor slot"),
		Equipment.Equip(EItemSlot::Armor, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::SlotMismatch);
	TestTrue(TEXT("a weapon instance cannot enter the accessory slot"),
		Equipment.Equip(EItemSlot::Accessory, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::SlotMismatch);
	TestTrue(TEXT("an armor instance cannot enter the weapon slot"),
		Equipment.Equip(EItemSlot::Weapon, Armor.InstanceId, Inventory) == EEquipmentEquipResult::SlotMismatch);
	TestTrue(TEXT("an accessory instance cannot enter the weapon slot"),
		Equipment.Equip(EItemSlot::Weapon, Charm.InstanceId, Inventory) == EEquipmentEquipResult::SlotMismatch);
	TestTrue(TEXT("an instance with an unknown DefinitionId cannot confirm any slot"),
		Equipment.Equip(EItemSlot::Accessory, Ghost.InstanceId, Inventory) == EEquipmentEquipResult::SlotMismatch);

	TestEqual(TEXT("no slot mapping was created"), Equipment.NumEquippedSlots(), 0);
	TestEqual(TEXT("all four instances are still stored"), Inventory.Count(), 4);
	TestTrue(TEXT("every instance survived the failed equips"),
		Inventory.Contains(Weapon.InstanceId) && Inventory.Contains(Armor.InstanceId) &&
		Inventory.Contains(Charm.InstanceId) && Inventory.Contains(Ghost.InstanceId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004EquipRejectsInvalidSlotAndMissingCatalog,
	"UEMMO.Tasks.M3_004.EquipRejectsInvalidSlotAndMissingCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004EquipRejectsInvalidSlotAndMissingCatalog::RunTest(const FString& Parameters)
{
	// Out-of-enum slot values and a missing definition source are separate,
	// explicit failures; the slot-match check is never bypassed silently.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_004Instance(0x31u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	if (!AddM3_004OrReport(*this, Inventory, Weapon, TEXT("Weapon")))
	{
		return true;
	}

	// The instance IS stored and the catalog IS bound, so InvalidSlot must
	// outrank every other reason: the closed slot set is checked first.
	const EItemSlot BogusSlot = static_cast<EItemSlot>(200);
	TestTrue(TEXT("an out-of-enum slot value is rejected as InvalidSlot"),
		Equipment.Equip(BogusSlot, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::InvalidSlot);

	// A model without any bound catalog cannot confirm a slot match, so it
	// fails with MissingDefinitions instead of equipping unchecked.
	FEquipmentModel NoCatalogEquipment;
	TestTrue(TEXT("equipping without a bound catalog returns MissingDefinitions"),
		NoCatalogEquipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::MissingDefinitions);

	TestEqual(TEXT("the guarded model stored no mapping"), Equipment.NumEquippedSlots(), 0);
	TestEqual(TEXT("the unbound model stored no mapping"), NoCatalogEquipment.NumEquippedSlots(), 0);
	TestEqual(TEXT("the inventory still holds exactly the weapon"), Inventory.Count(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004SameSlotReplacementKeepsSingleActiveItem,
	"UEMMO.Tasks.M3_004.SameSlotReplacementKeepsSingleActiveItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004SameSlotReplacementKeepsSingleActiveItem::RunTest(const FString& Parameters)
{
	// Equipping a second weapon into the occupied weapon slot replaces only
	// the id mapping: the slot then references the new id, the old item stays
	// stored in the inventory, and exactly one weapon is active.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance FirstWeapon = MakeM3_004Instance(0x41u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance SecondWeapon = MakeM3_004Instance(0x42u, TEXT("weapon_training"), 8.0f, 0.0f, 0.0f, 2);
	if (!AddM3_004OrReport(*this, Inventory, FirstWeapon, TEXT("FirstWeapon")) ||
		!AddM3_004OrReport(*this, Inventory, SecondWeapon, TEXT("SecondWeapon")))
	{
		return true;
	}

	TestTrue(TEXT("equipping the first weapon succeeds"),
		Equipment.Equip(EItemSlot::Weapon, FirstWeapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	const FGuid* EquippedFirst = Equipment.GetEquippedId(EItemSlot::Weapon);
	TestTrue(TEXT("the weapon slot maps to the first weapon"),
		EquippedFirst != nullptr && *EquippedFirst == FirstWeapon.InstanceId);

	TestTrue(TEXT("equipping the second weapon into the same slot succeeds (replacement)"),
		Equipment.Equip(EItemSlot::Weapon, SecondWeapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);

	const FGuid* EquippedSecond = Equipment.GetEquippedId(EItemSlot::Weapon);
	TestTrue(TEXT("the weapon slot now maps to the second weapon"),
		EquippedSecond != nullptr && *EquippedSecond == SecondWeapon.InstanceId);
	TestTrue(TEXT("the first weapon is no longer equipped"),
		!Equipment.IsInstanceEquipped(FirstWeapon.InstanceId));
	TestTrue(TEXT("the second weapon is the one equipped instance"),
		Equipment.IsInstanceEquipped(SecondWeapon.InstanceId));
	TestEqual(TEXT("exactly one slot is occupied after the replacement"), Equipment.NumEquippedSlots(), 1);

	// Replacement only changes the mapping: nothing was added or deleted.
	TestEqual(TEXT("both weapons stay stored in the inventory"), Inventory.Count(), 2);
	TestTrue(TEXT("the first weapon is still stored after being replaced"),
		Inventory.Contains(FirstWeapon.InstanceId));
	TestTrue(TEXT("the second weapon is stored"), Inventory.Contains(SecondWeapon.InstanceId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004DuplicateEquipIsIdempotentWithoutDoubleDeduction,
	"UEMMO.Tasks.M3_004.DuplicateEquipIsIdempotentWithoutDoubleDeduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004DuplicateEquipIsIdempotentWithoutDoubleDeduction::RunTest(const FString& Parameters)
{
	// Equipping the same instance into its already occupied slot is the
	// idempotent AlreadyEquipped no-op: the mapping stays a single entry (no
	// stat stacking) and the inventory is not touched again (no deduction).
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_004Instance(0x51u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance Charm = MakeM3_004Instance(0x52u, TEXT("charm_training"), 0.0f, 0.0f, 20.0f, 1);
	if (!AddM3_004OrReport(*this, Inventory, Weapon, TEXT("Weapon")) ||
		!AddM3_004OrReport(*this, Inventory, Charm, TEXT("Charm")))
	{
		return true;
	}

	TestTrue(TEXT("the first equip succeeds"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	TestTrue(TEXT("re-equipping the same instance into the same slot returns AlreadyEquipped"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::AlreadyEquipped);

	const FGuid* Equipped = Equipment.GetEquippedId(EItemSlot::Weapon);
	TestTrue(TEXT("the weapon slot still maps to the same instance"),
		Equipped != nullptr && *Equipped == Weapon.InstanceId);
	TestEqual(TEXT("the mapping is still a single entry"), Equipment.NumEquippedSlots(), 1);
	TestTrue(TEXT("no second mapping appeared in another slot"),
		Equipment.GetEquippedId(EItemSlot::Armor) == nullptr &&
		Equipment.GetEquippedId(EItemSlot::Accessory) == nullptr);
	TestEqual(TEXT("the duplicate equip did not deduct or add anything"), Inventory.Count(), 2);
	TestTrue(TEXT("the equipped instance is still stored exactly once"),
		Inventory.Contains(Weapon.InstanceId) && !Inventory.Contains(FGuid()));

	// Idempotency survives intervening equips into other slots.
	TestTrue(TEXT("equipping the charm succeeds"),
		Equipment.Equip(EItemSlot::Accessory, Charm.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	TestTrue(TEXT("after another equip the same weapon re-equip is still AlreadyEquipped"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::AlreadyEquipped);
	TestEqual(TEXT("two slots are occupied by the two distinct instances"), Equipment.NumEquippedSlots(), 2);
	TestEqual(TEXT("the inventory count is unchanged by all the equips"), Inventory.Count(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004InventoryRemoveRejectsEquippedInstance,
	"UEMMO.Tasks.M3_004.InventoryRemoveRejectsEquippedInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004InventoryRemoveRejectsEquippedInstance::RunTest(const FString& Parameters)
{
	// The inventory's equip guard: removing an instance that an equipment
	// model currently references returns ItemEquipped as a pure no-op; items
	// that are not equipped stay freely removable; unequip releases the guard.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_004Instance(0x61u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance Charm = MakeM3_004Instance(0x62u, TEXT("charm_training"), 0.0f, 0.0f, 20.0f, 1);
	if (!AddM3_004OrReport(*this, Inventory, Weapon, TEXT("Weapon")) ||
		!AddM3_004OrReport(*this, Inventory, Charm, TEXT("Charm")))
	{
		return true;
	}
	TestTrue(TEXT("setup: equipping the weapon succeeds"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);

	const FGuid Stranger(0xDEADu, 0xBEEFu, 0xCAFEu, 0xF00Du);
	TestTrue(TEXT("removing the equipped instance returns ItemEquipped"),
		Inventory.Remove(Weapon.InstanceId) == EInventoryRemoveResult::ItemEquipped);
	TestEqual(TEXT("the refused removal left both items stored"), Inventory.Count(), 2);
	TestTrue(TEXT("the refused removal kept the insertion order"),
		Inventory.GetByIndex(0) != nullptr && Inventory.GetByIndex(0)->InstanceId == Weapon.InstanceId &&
		Inventory.GetByIndex(1) != nullptr && Inventory.GetByIndex(1)->InstanceId == Charm.InstanceId);
	const FGuid* Equipped = Equipment.GetEquippedId(EItemSlot::Weapon);
	TestTrue(TEXT("the refused removal left the equipment mapping intact"),
		Equipped != nullptr && *Equipped == Weapon.InstanceId);

	TestTrue(TEXT("an unknown id still reports NotFound"), Inventory.Remove(Stranger) == EInventoryRemoveResult::NotFound);
	TestTrue(TEXT("the unequipped charm is still freely removable"),
		Inventory.Remove(Charm.InstanceId) == EInventoryRemoveResult::Removed);
	TestEqual(TEXT("only the charm was removed"), Inventory.Count(), 1);

	// Unequip releases the guard: the weapon can then be removed, re-added
	// and re-equipped, proving no stale guard state lingers.
	TestTrue(TEXT("unequipping the weapon succeeds"),
		Equipment.Unequip(EItemSlot::Weapon) == EEquipmentUnequipResult::Unequipped);
	TestTrue(TEXT("after unequip the weapon is removable"),
		Inventory.Remove(Weapon.InstanceId) == EInventoryRemoveResult::Removed);
	TestEqual(TEXT("the inventory is empty after both removals"), Inventory.Count(), 0);
	TestTrue(TEXT("the weapon can be added again"), Inventory.TryAdd(Weapon) == EInventoryAddResult::Added);
	TestTrue(TEXT("the re-added weapon can be equipped again"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004UnequipClearsSlotAndAllowsReEquip,
	"UEMMO.Tasks.M3_004.UnequipClearsSlotAndAllowsReEquip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004UnequipClearsSlotAndAllowsReEquip::RunTest(const FString& Parameters)
{
	// Unequipping clears only the id mapping: the item was never moved, so it
	// remains stored and can be equipped again. Empty-slot and invalid-slot
	// unequips are explicit idempotent no-ops.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_004Instance(0x71u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance Armor = MakeM3_004Instance(0x72u, TEXT("armor_training"), 0.0f, 3.0f, 0.0f, 1);
	const FItemInstance Charm = MakeM3_004Instance(0x73u, TEXT("charm_training"), 0.0f, 0.0f, 20.0f, 1);
	if (!AddM3_004OrReport(*this, Inventory, Weapon, TEXT("Weapon")) ||
		!AddM3_004OrReport(*this, Inventory, Armor, TEXT("Armor")) ||
		!AddM3_004OrReport(*this, Inventory, Charm, TEXT("Charm")))
	{
		return true;
	}
	TestTrue(TEXT("setup: the weapon equips"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	TestTrue(TEXT("setup: the armor equips"),
		Equipment.Equip(EItemSlot::Armor, Armor.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);

	TestTrue(TEXT("unequipping the weapon clears the slot"),
		Equipment.Unequip(EItemSlot::Weapon) == EEquipmentUnequipResult::Unequipped);
	TestTrue(TEXT("the weapon slot reads as empty"), Equipment.GetEquippedId(EItemSlot::Weapon) == nullptr);
	TestTrue(TEXT("the weapon slot reports not equipped"), !Equipment.IsSlotEquipped(EItemSlot::Weapon));
	TestTrue(TEXT("the weapon instance is no longer reported equipped"),
		!Equipment.IsInstanceEquipped(Weapon.InstanceId));
	TestEqual(TEXT("the armor slot was untouched by the unequip"), Equipment.NumEquippedSlots(), 1);

	// Unequip moves nothing: the item simply stays stored.
	TestEqual(TEXT("all three items are still stored"), Inventory.Count(), 3);
	TestTrue(TEXT("the unequipped weapon is still stored"), Inventory.Contains(Weapon.InstanceId));

	TestTrue(TEXT("unequipping an already empty slot is the NotEquipped no-op"),
		Equipment.Unequip(EItemSlot::Weapon) == EEquipmentUnequipResult::NotEquipped);
	const EItemSlot BogusSlot = static_cast<EItemSlot>(200);
	TestTrue(TEXT("unequipping an out-of-enum slot is InvalidSlot"),
		Equipment.Unequip(BogusSlot) == EEquipmentUnequipResult::InvalidSlot);

	// The cleared slot accepts the same instance again.
	TestTrue(TEXT("the same weapon can be equipped again after unequip"),
		Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);

	// Full teardown: every closed slot unequips cleanly.
	TestTrue(TEXT("the re-equipped weapon unequips"),
		Equipment.Unequip(EItemSlot::Weapon) == EEquipmentUnequipResult::Unequipped);
	TestTrue(TEXT("the armor unequips"),
		Equipment.Unequip(EItemSlot::Armor) == EEquipmentUnequipResult::Unequipped);
	TestTrue(TEXT("setup: the charm equips"),
		Equipment.Equip(EItemSlot::Accessory, Charm.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	TestTrue(TEXT("the charm unequips"),
		Equipment.Unequip(EItemSlot::Accessory) == EEquipmentUnequipResult::Unequipped);
	TestEqual(TEXT("no slot is occupied after the full teardown"), Equipment.NumEquippedSlots(), 0);
	TestEqual(TEXT("the inventory still holds all three items"), Inventory.Count(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_004ReplacementLeavesInstancePayloadsUntouched,
	"UEMMO.Tasks.M3_004.ReplacementLeavesInstancePayloadsUntouched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_004ReplacementLeavesInstancePayloadsUntouched::RunTest(const FString& Parameters)
{
	// Replacing one weapon with another must not modify either instance's
	// inherent data: DefinitionId, RollSeed, Level and all three rolled stats
	// stay exactly as they were for both instances.
	const FItemDefinitionCatalog Catalog = MakeM3_004Catalog();
	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance FirstWeapon = MakeM3_004Instance(0x81u, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance SecondWeapon = MakeM3_004Instance(0x82u, TEXT("weapon_training"), 7.0f, 1.5f, 2.0f, 3);
	const FItemInstance FirstBefore = FirstWeapon;
	const FItemInstance SecondBefore = SecondWeapon;
	if (!AddM3_004OrReport(*this, Inventory, FirstWeapon, TEXT("FirstWeapon")) ||
		!AddM3_004OrReport(*this, Inventory, SecondWeapon, TEXT("SecondWeapon")))
	{
		return true;
	}

	TestTrue(TEXT("setup: the first weapon equips"),
		Equipment.Equip(EItemSlot::Weapon, FirstWeapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
	TestTrue(TEXT("setup: the second weapon replaces it"),
		Equipment.Equip(EItemSlot::Weapon, SecondWeapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);

	const FItemInstance* StoredFirst = FindM3_004ById(Inventory, FirstWeapon.InstanceId);
	const FItemInstance* StoredSecond = FindM3_004ById(Inventory, SecondWeapon.InstanceId);
	TestTrue(TEXT("the replaced first weapon is still stored"), StoredFirst != nullptr);
	TestTrue(TEXT("the equipping second weapon is stored"), StoredSecond != nullptr);
	if (!StoredFirst || !StoredSecond)
	{
		return true;
	}

	TestTrue(TEXT("the replaced first weapon's payload is fully unchanged"),
		M3_004SamePayload(*StoredFirst, FirstBefore));
	TestTrue(TEXT("the newly equipped second weapon's payload is fully unchanged"),
		M3_004SamePayload(*StoredSecond, SecondBefore));
	TestEqual(TEXT("the first weapon keeps its DefinitionId"),
		StoredFirst->DefinitionId, FName(TEXT("weapon_training")));
	TestEqual(TEXT("the first weapon keeps its Attack"), StoredFirst->RolledStats.Attack, 5.0f);
	TestEqual(TEXT("the second weapon keeps its Level"), StoredSecond->Level, 3);
	TestEqual(TEXT("the second weapon keeps its RollSeed"), StoredSecond->RollSeed, static_cast<int64>(0x82u));
	TestEqual(TEXT("the second weapon keeps its MaxHP"), StoredSecond->RolledStats.MaxHP, 2.0f);

	// And the mapping references exactly one of the two untouched instances.
	TestTrue(TEXT("exactly the second weapon is equipped"),
		Equipment.IsInstanceEquipped(SecondWeapon.InstanceId) && !Equipment.IsInstanceEquipped(FirstWeapon.InstanceId));
	TestEqual(TEXT("both instances remain stored"), Inventory.Count(), 2);
	return true;
}

#endif
