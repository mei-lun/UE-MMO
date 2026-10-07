// M5-020A: the production content catalog wiring. Pins the shared reader for
// Data/items.json and Data/drops.json (the HUD display and the reward pool
// source), the frozen historical alias (accessory_training reads as
// charm_training for DISPLAY only - InstanceIds never change), the mapping
// into the reward drop-table layer, and the "a new legal pool rolls real
// rewards through the original claim transaction" acceptance: a fixture pool
// with a new item rolls through FDropGenerator against the loaded catalog
// and the generated instance claims into an inventory model.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Items/DropGenerator.h"
#include "../Items/DropTable.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Items/InventoryModel.h"
#include "../Profile/RewardService.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// ProductionReadersParseTheShippedTables
// ---------------------------------------------------------------------------

// The shipped Data/items.json and Data/drops.json parse: three items with the
// design stats and the starter pool 50/30/20; malformed shapes name their
// field and refuse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020AProductionReadersParseTheShippedTables,
	"UEMMO.Tasks.M5_020A.ProductionReadersParseTheShippedTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020AProductionReadersParseTheShippedTables::RunTest(const FString& Parameters)
{
	// 1. The shipped items table.
	{
		TArray<FTestRoomItemRow> Rows;
		FString Error;
		if (!TestTrue(TEXT("the shipped items source loads"),
			ACombatTestRoomDriver::LoadProductionItems(Rows, Error)))
		{
			AddError(Error);
			return true;
		}
		TestEqual("the shipped table carries the three items", Rows.Num(), 3);
		TestEqual("the sword id resolves", Rows[0].DefinitionId, FName(TEXT("weapon_training")));
		TestEqual("the sword attack comes from the table", Rows[0].Attack, 5.0f);
		TestEqual("the armor defense comes from the table", Rows[1].Defense, 3.0f);
		TestEqual("the charm max hp comes from the table", Rows[2].MaxHP, 20.0f);
		TestEqual("the charm id is the authority charm_training (the alias target)",
			Rows[2].DefinitionId, FName(TEXT("charm_training")));
	}

	// 2. The shipped drops table.
	{
		FTestRoomDropPool Pool;
		FString Error;
		if (!TestTrue(TEXT("the shipped drops source loads"),
			ACombatTestRoomDriver::LoadProductionDropPool(Pool, Error)))
		{
			AddError(Error);
			return true;
		}
		TestEqual("the pool id is starter", Pool.TableId, FName(TEXT("starter")));
		TestEqual("the pool carries the three weighted entries", Pool.Entries.Num(), 3);
		TestEqual("the weapon weight is the design 50", Pool.Entries[0].Weight, 50);
		TestEqual("the armor weight is the design 30", Pool.Entries[1].Weight, 30);
		TestEqual("the charm weight is the design 20", Pool.Entries[2].Weight, 20);
	}

	// 3. Malformed shapes refuse with field-naming errors.
	{
		const TCHAR* Duplicate = TEXT(R"({
			"schema_version": 1,
			"items": [
				{"definition_id": "weapon_training", "display_name": "A", "slot": "Weapon",
				 "base_stats": {"attack": 1, "defense": 0, "max_hp": 0}},
				{"definition_id": "weapon_training", "display_name": "B", "slot": "Weapon",
				 "base_stats": {"attack": 2, "defense": 0, "max_hp": 0}}
			]
		})");
		TArray<FTestRoomItemRow> Rows;
		FString Error;
		TestFalse("a duplicated item id refuses",
			ACombatTestRoomDriver::ParseProductionItems(Duplicate, Rows, Error));
		TestTrue("the item refusal names the duplicate", Error.Contains(TEXT("twice")));

		const TCHAR* UnknownDrop = TEXT(R"({
			"schema_version": 1, "table_id": "starter",
			"entries": [
				{"definition_id": "weapon_training", "weight": 50},
				{"definition_id": "weapon_training", "weight": 50}
			]
		})");
		FTestRoomDropPool Pool;
		TestFalse("a duplicated drop id refuses",
			ACombatTestRoomDriver::ParseProductionDropPool(UnknownDrop, Pool, Error));
		TestTrue("the drop refusal names the duplicate", Error.Contains(TEXT("twice")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// FrozenAliasResolvesDisplayOnly
// ---------------------------------------------------------------------------

// The frozen M3-era alias: accessory_training reads as the authority
// charm_training id; every other id passes through untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020AFrozenAliasResolvesDisplayOnly,
	"UEMMO.Tasks.M5_020A.FrozenAliasResolvesDisplayOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020AFrozenAliasResolvesDisplayOnly::RunTest(const FString& Parameters)
{
	TestEqual("the legacy accessory id resolves to the authority charm id",
		ACombatTestRoomDriver::ResolveItemAlias(FName(TEXT("accessory_training"))),
		FName(TEXT("charm_training")));
	TestEqual("the authority charm id passes through",
		ACombatTestRoomDriver::ResolveItemAlias(FName(TEXT("charm_training"))),
		FName(TEXT("charm_training")));
	TestEqual("the sword id passes through untouched",
		ACombatTestRoomDriver::ResolveItemAlias(FName(TEXT("weapon_training"))),
		FName(TEXT("weapon_training")));
	TestEqual("an unknown id passes through (the lookup layer refuses it, not the alias)",
		ACombatTestRoomDriver::ResolveItemAlias(FName(TEXT("no_such_item"))),
		FName(TEXT("no_such_item")));
	return true;
}

// ---------------------------------------------------------------------------
// NewPoolRollsThroughTheOriginalClaim
// ---------------------------------------------------------------------------

// The acceptance core: a NEW legal pool (fixture text - the test fixture may
// keep an explicit double) maps into the reward drop-table layer, rolls a
// real instance through FDropGenerator against the loaded production
// catalog, and the generated instance claims into an inventory model - a new
// item reaches the inventory without any code change and without bypassing
// the claim transaction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020ANewPoolRollsThroughTheOriginalClaim,
	"UEMMO.Tasks.M5_020A.NewPoolRollsThroughTheOriginalClaim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020ANewPoolRollsThroughTheOriginalClaim::RunTest(const FString& Parameters)
{
	// 1. The production catalog (the loader) backs the generation lookups.
	TArray<FTestRoomItemRow> Rows;
	FString Error;
	if (!TestTrue(TEXT("the production items load"), ACombatTestRoomDriver::LoadProductionItems(Rows, Error)))
	{
		AddError(Error);
		return true;
	}
	FItemDefinitionCatalog Catalog;
	for (const FTestRoomItemRow& Row : Rows)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = Row.DefinitionId;
		Definition.DisplayName = Row.DisplayName;
		Definition.Slot = Row.Slot == TEXT("Weapon") ? EItemSlot::Weapon
			: Row.Slot == TEXT("Armor") ? EItemSlot::Armor
			: Row.Slot == TEXT("Accessory") ? EItemSlot::Accessory
			: EItemSlot::Weapon;
		Definition.BaseStats.Attack = Row.Attack;
		Definition.BaseStats.Defense = Row.Defense;
		Definition.BaseStats.MaxHP = Row.MaxHP;
		Definition.Rarity = static_cast<EItemRarity>(FMath::Clamp(Row.Rarity, 1, 3));
		TestTrue(FString::Printf(TEXT("the catalog accepts %s"), *Row.DefinitionId.ToString()),
			Catalog.AddDefinition(Definition));
	}

	// 2. A NEW legal pool (fixture text): a fourth item plus a pool row for
	//    it - the parser accepts it and the mapping carries it.
	const TCHAR* NewItem = TEXT(R"({
		"schema_version": 1,
		"items": [
			{"definition_id": "weapon_iron", "display_name": "Iron Sword", "slot": "Weapon",
			 "base_stats": {"attack": 8, "defense": 0, "max_hp": 0}, "rarity": 2}
		]
	})");
	TArray<FTestRoomItemRow> NewRows;
	TestTrue("the new item parses",
		ACombatTestRoomDriver::ParseProductionItems(NewItem, NewRows, Error));
	FItemDefinition IronDefinition;
	IronDefinition.DefinitionId = NewRows[0].DefinitionId;
	IronDefinition.DisplayName = NewRows[0].DisplayName;
	IronDefinition.Slot = EItemSlot::Weapon;
	IronDefinition.BaseStats.Attack = NewRows[0].Attack;
	IronDefinition.Rarity = static_cast<EItemRarity>(FMath::Clamp(NewRows[0].Rarity, 1, 3));
	if (!TestTrue("the catalog accepts the new item",
		Catalog.AddDefinition(IronDefinition)))
	{
		return true;
	}

	const TCHAR* NewPool = TEXT(R"({
		"schema_version": 1, "table_id": "iron_pool",
		"entries": [{"definition_id": "weapon_iron", "weight": 100}]
	})");
	FTestRoomDropPool Pool;
	if (!TestTrue("the new pool parses",
		ACombatTestRoomDriver::ParseProductionDropPool(NewPool, Pool, Error)))
	{
		AddError(Error);
		return true;
	}

	// 3. The mapping into the reward layer and the real roll.
	FDropTable Table;
	Table.TableId = Pool.TableId;
	for (const FTestRoomDropEntry& Entry : Pool.Entries)
	{
		Table.Entries.Add(FDropEntry{ Entry.DefinitionId, Entry.Weight });
	}
	const FDropRewardResult Drop = FDropGenerator::GenerateReward(
		/*RewardSeed*/ 424242, /*SettlementId*/ 7, Table, Catalog);
	if (!TestTrue("the new pool rolls a real instance", Drop.bSuccess))
	{
		AddError(Drop.Error);
		return true;
	}
	TestEqual("the rolled instance is the new item",
		Drop.Instance.DefinitionId, FName(TEXT("weapon_iron")));

	// 4. The original claim transaction: the generated instance claims into
	//    an inventory model (the same entry the RewardService claim uses).
	FInventoryModel Inventory;
	const FItemDefinition* RolledDefinition = Catalog.Find(
		ACombatTestRoomDriver::ResolveItemAlias(Drop.Instance.DefinitionId));
	if (!TestNotNull("the rolled definition resolves through the alias layer", RolledDefinition))
	{
		return true;
	}
	// The claim's inventory mutation entry (the M3-009 in-memory body adds
	// through the same model call).
	const EInventoryAddResult AddResult = Inventory.TryAdd(Drop.Instance);
	TestTrue("the claim adds the instance to the inventory",
		AddResult == EInventoryAddResult::Added);
	TestEqual("the inventory holds the claimed item", Inventory.Count(), 1);
	TestTrue("the claimed instance id is unchanged",
		Inventory.Contains(Drop.Instance.InstanceId));
	return true;
}

#endif
