// M5-019: the session weapon binding layer (M5 interface contract section 1
// owner 019, section 7 "存档与实例"). Pins Weapons/WeaponBinding.h:
//
// - The item_definition_id chain resolves an FItemInstance to its weapon
//   behavior: FItemInstance --DefinitionId--> FItemDefinition
//   --WeaponDefinitionId--> FWeaponDefinition (FCombatCatalog FindWeapon).
//   The binding copies InstanceId/Level/RollSeed verbatim - no second item
//   instance is ever created.
// - The magazine slot is keyed by ItemInstanceId: two instances of the same
//   weapon definition never share loaded rounds. Unbind parks the rounds and
//   a rebind restores them (switching equipment does not initialize new ammo);
//   a first bind starts full. The exact loaded/reserve/reload semantics are
//   M5-021's AmmoModel.
// - Rejections are explicit and named (invalid instance, duplicate active
//   binding, unknown item definition, non-weapon slot, missing mapping,
//   unknown weapon id); the registry stays unchanged and there is never a
//   training-sword fallback.
// - The M5-019 ItemDefinition.h addition: WeaponDefinitionId is only legal on
//   Weapon-slot definitions and must match the M5 id text rule; armor/charm
//   definitions without the field validate exactly as before (M3 unchanged).
//
// Pure logic only: no World, no UE assets, no wall clocks.

#include "Misc/AutomationTest.h"

#include "../Weapons/WeaponBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_019
{
	/** Minimal combat catalog carrying the two weapon rows the tests bind to. */
	bool MakeCombatCatalog(FCombatCatalog& OutCatalog, FString& OutError)
	{
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;

		// The damage profiles the two weapons reference (values irrelevant to
		// binding; ids must exist for faithfulness with the source tables).
		FDamageProfile Light01;
		Light01.DamageProfileId = TEXT("light_01");
		Light01.BaseDamage = 10.0f;
		Light01.AttackCoefficient = 1.0f;
		Light01.HitStunSeconds = 0.22f;
		Light01.KnockbackCmPerSecond = 90.0f;
		Light01.HitStopSeconds = 0.04f;
		Parsed.DamageProfiles.Add(Light01);

		FDamageProfile Light02 = Light01;
		Light02.DamageProfileId = TEXT("light_02");
		Light02.BaseDamage = 14.0f;
		Light02.HitStunSeconds = 0.28f;
		Parsed.DamageProfiles.Add(Light02);

		// The attack reactions of the legacy melee set.
		for (const FName AttackId : GetLegacyMeleeAttackIds())
		{
			FAttackReaction Row;
			Row.ReactionId = AttackId;
			Row.ControlPenetration = EControlPenetration::None;
			Parsed.AttackReactions.Add(Row);
		}

		// The weapon rows under test (weapons.json weapon_training_sword /
		// weapon_hitscan_sample).
		FWeaponDefinition Sword;
		Sword.WeaponId = TEXT("weapon_training_sword");
		Sword.FireMode = EWeaponFireMode::Melee;
		Sword.DamageProfileId = TEXT("light_01");
		Sword.MeleeAttackIds = GetLegacyMeleeAttackIds();
		Parsed.Weapons.Add(Sword);

		FWeaponDefinition Hitscan;
		Hitscan.WeaponId = TEXT("weapon_hitscan_sample");
		Hitscan.FireMode = EWeaponFireMode::Hitscan;
		Hitscan.DamageProfileId = TEXT("light_02");
		Hitscan.AmmoId = TEXT("ammo_cell");
		Hitscan.MagazineSize = 12;
		Hitscan.FireRateRpm = 240.0f;
		Hitscan.SpreadDegrees = 1.0f;
		Hitscan.RangeCm = 5000.0f;
		Parsed.Weapons.Add(Hitscan);

		// The shared ammo kind of the hitscan row.
		FAmmoType Cell;
		Cell.AmmoId = TEXT("ammo_cell");
		Cell.MaxReserve = 120;
		Cell.MagazineSize = 12;
		Parsed.AmmoTypes.Add(Cell);

		return FCombatCatalog::BuildFromParsed(Parsed, OutCatalog, OutError);
	}

	/** Item id constants shared by the tests. */
	const FName ItemTrainingSword = TEXT("weapon_training");
	const FName ItemRangedTest = TEXT("weapon_ranged_test");
	const FName ItemUnmapped = TEXT("weapon_unmapped");
	const FName ItemBadMapping = TEXT("weapon_bad_mapping");
	const FName ItemArmor = TEXT("armor_training");
	const FName ItemCharm = TEXT("charm_training");

	/**
	 * The item definition catalog of the session: the training sword and a
	 * ranged test item carry weapon mappings; an unmapped weapon item, a
	 * wrongly mapped weapon item, the training armor and the training charm
	 * complete the M3 shape.
	 */
	FItemDefinitionCatalog MakeItemCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition Sword;
		Sword.DefinitionId = ItemTrainingSword;
		Sword.DisplayName = TEXT("Training Sword");
		Sword.Slot = EItemSlot::Weapon;
		Sword.BaseStats.Attack = 5.0f;
		Sword.Rarity = EItemRarity::Normal;
		Sword.WeaponDefinitionId = TEXT("weapon_training_sword");
		Catalog.AddDefinition(Sword);

		FItemDefinition Ranged;
		Ranged.DefinitionId = ItemRangedTest;
		Ranged.DisplayName = TEXT("Ranged Test Gun");
		Ranged.Slot = EItemSlot::Weapon;
		Ranged.BaseStats.Attack = 2.0f;
		Ranged.Rarity = EItemRarity::Normal;
		Ranged.WeaponDefinitionId = TEXT("weapon_hitscan_sample");
		Catalog.AddDefinition(Ranged);

		FItemDefinition Unmapped;
		Unmapped.DefinitionId = ItemUnmapped;
		Unmapped.DisplayName = TEXT("Unmapped Weapon Item");
		Unmapped.Slot = EItemSlot::Weapon;
		Unmapped.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Unmapped);

		FItemDefinition BadMapping;
		BadMapping.DefinitionId = ItemBadMapping;
		BadMapping.DisplayName = TEXT("Wrongly Mapped Weapon Item");
		BadMapping.Slot = EItemSlot::Weapon;
		BadMapping.Rarity = EItemRarity::Normal;
		BadMapping.WeaponDefinitionId = TEXT("weapon_does_not_exist");
		Catalog.AddDefinition(BadMapping);

		FItemDefinition Armor;
		Armor.DefinitionId = ItemArmor;
		Armor.DisplayName = TEXT("Training Armor");
		Armor.Slot = EItemSlot::Armor;
		Armor.BaseStats.Defense = 3.0f;
		Armor.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Armor);

		FItemDefinition Charm;
		Charm.DefinitionId = ItemCharm;
		Charm.DisplayName = TEXT("Training Charm");
		Charm.Slot = EItemSlot::Accessory;
		Charm.BaseStats.MaxHP = 20.0f;
		Charm.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Charm);

		return Catalog;
	}

	/** One instance of DefinitionId with a fresh identity and the given seed. */
	FItemInstance MakeInstance(FName DefinitionId, int64 RollSeed, int32 Level = 1)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid::NewGuid();
		Instance.DefinitionId = DefinitionId;
		Instance.RollSeed = RollSeed;
		Instance.RolledStats.Attack = 5.0f;
		Instance.Level = Level;
		return Instance;
	}
}

using namespace UE::UEMMO::Tasks::M5_019;

/**
 * The training sword and the ranged test item resolve through the
 * item_definition_id chain, and the binding preserves the original
 * FGuid/Level/RollSeed identity instead of minting a second instance.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019TrainingSwordAndRangedBind,
	"UEMMO.Tasks.M5_019.TrainingSwordAndRangedBind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019TrainingSwordAndRangedBind::RunTest(const FString& Parameters)
{
	FCombatCatalog Weapons;
	FString Error;
	if (!MakeCombatCatalog(Weapons, Error))
	{
		AddError(FString::Printf(TEXT("combat catalog build failed: %s"), *Error));
		return false;
	}
	const FItemDefinitionCatalog Items = MakeItemCatalog();
	FWeaponBindingRegistry Registry;

	// Melee: the training sword binds with no magazine.
	const FItemInstance SwordInstance = MakeInstance(ItemTrainingSword, 4242, 3);
	const FWeaponBindOutcome SwordBind = Registry.BindInstance(SwordInstance, Items, Weapons);
	TestTrue(TEXT("training sword binds"), SwordBind.bBound);
	if (SwordBind.bBound)
	{
		TestEqual(TEXT("sword instance id preserved"), SwordBind.Record.InstanceId, SwordInstance.InstanceId);
		TestEqual(TEXT("sword level preserved"), SwordBind.Record.Level, 3);
		TestEqual(TEXT("sword roll seed preserved"), SwordBind.Record.RollSeed, static_cast<int64>(4242));
		TestEqual(TEXT("sword item anchor"), SwordBind.Record.ItemDefinitionId, ItemTrainingSword);
		TestEqual(TEXT("sword resolves weapon_training_sword"), SwordBind.Record.WeaponDefinitionId, FName(TEXT("weapon_training_sword")));
		TestEqual(TEXT("sword fire mode melee"), SwordBind.Record.FireMode, EWeaponFireMode::Melee);
		TestEqual(TEXT("sword has no magazine"), SwordBind.Record.MagazineCapacity, 0);
		TestEqual(TEXT("sword loaded rounds 0"), SwordBind.Record.LoadedRounds, 0);
	}

	// Ranged: the test gun binds with a full first magazine.
	const FItemInstance GunInstance = MakeInstance(ItemRangedTest, 777, 1);
	const FWeaponBindOutcome GunBind = Registry.BindInstance(GunInstance, Items, Weapons);
	TestTrue(TEXT("ranged test item binds"), GunBind.bBound);
	if (GunBind.bBound)
	{
		TestEqual(TEXT("gun instance id preserved"), GunBind.Record.InstanceId, GunInstance.InstanceId);
		TestEqual(TEXT("gun roll seed preserved"), GunBind.Record.RollSeed, static_cast<int64>(777));
		TestEqual(TEXT("gun resolves weapon_hitscan_sample"), GunBind.Record.WeaponDefinitionId, FName(TEXT("weapon_hitscan_sample")));
		TestEqual(TEXT("gun fire mode hitscan"), GunBind.Record.FireMode, EWeaponFireMode::Hitscan);
		TestEqual(TEXT("gun magazine capacity 12"), GunBind.Record.MagazineCapacity, 12);
		TestEqual(TEXT("first bind starts full"), GunBind.Record.LoadedRounds, 12);
	}

	TestEqual(TEXT("two active bindings"), Registry.NumBindings(), 2);
	return true;
}

/**
 * Two instances of the same weapon definition hold separate magazine slots:
 * spending rounds on one never leaks into the other.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019SameDefinitionInstancesSeparateMagazines,
	"UEMMO.Tasks.M5_019.SameDefinitionInstancesSeparateMagazines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019SameDefinitionInstancesSeparateMagazines::RunTest(const FString& Parameters)
{
	FCombatCatalog Weapons;
	FString Error;
	if (!MakeCombatCatalog(Weapons, Error))
	{
		AddError(FString::Printf(TEXT("combat catalog build failed: %s"), *Error));
		return false;
	}
	const FItemDefinitionCatalog Items = MakeItemCatalog();
	FWeaponBindingRegistry Registry;

	const FItemInstance First = MakeInstance(ItemRangedTest, 1);
	const FItemInstance Second = MakeInstance(ItemRangedTest, 2);
	TestTrue(TEXT("first binds"), Registry.BindInstance(First, Items, Weapons).bBound);
	TestTrue(TEXT("second binds"), Registry.BindInstance(Second, Items, Weapons).bBound);
	TestTrue(TEXT("two instances of one definition coexist"), First.InstanceId != Second.InstanceId);

	// Spend rounds on the first instance only.
	TestTrue(TEXT("spend on first"), Registry.SetInstanceRounds(First.InstanceId, 5));

	const FWeaponBindingRecord* FirstRecord = Registry.FindBinding(First.InstanceId);
	const FWeaponBindingRecord* SecondRecord = Registry.FindBinding(Second.InstanceId);
	if (FirstRecord == nullptr || SecondRecord == nullptr)
	{
		AddError(TEXT("both bindings must stay resolvable"));
		return false;
	}
	TestEqual(TEXT("first instance spent to 5"), FirstRecord->LoadedRounds, 5);
	TestEqual(TEXT("second instance keeps its own full magazine"), SecondRecord->LoadedRounds, 12);
	TestEqual(TEXT("same definition anchor"), SecondRecord->WeaponDefinitionId, FirstRecord->WeaponDefinitionId);
	return true;
}

/**
 * Unequip/re-equip keeps the identity and the magazine: the rebind restores
 * the parked rounds instead of re-initializing a full magazine.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019RebindKeepsIdentityAndRounds,
	"UEMMO.Tasks.M5_019.RebindKeepsIdentityAndRounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019RebindKeepsIdentityAndRounds::RunTest(const FString& Parameters)
{
	FCombatCatalog Weapons;
	FString Error;
	if (!MakeCombatCatalog(Weapons, Error))
	{
		AddError(FString::Printf(TEXT("combat catalog build failed: %s"), *Error));
		return false;
	}
	const FItemDefinitionCatalog Items = MakeItemCatalog();
	FWeaponBindingRegistry Registry;

	const FItemInstance Instance = MakeInstance(ItemRangedTest, 90210, 4);
	TestTrue(TEXT("initial bind"), Registry.BindInstance(Instance, Items, Weapons).bBound);
	TestTrue(TEXT("partially spend"), Registry.SetInstanceRounds(Instance.InstanceId, 7));

	// Unequip.
	TestTrue(TEXT("unbind"), Registry.UnbindInstance(Instance.InstanceId));
	TestEqual(TEXT("no active binding after unbind"), Registry.NumBindings(), 0);
	TestEqual(TEXT("unbind again refuses"), Registry.UnbindInstance(Instance.InstanceId) ? 1 : 0, 0);

	// Re-equip the very same instance value.
	const FWeaponBindOutcome Rebind = Registry.BindInstance(Instance, Items, Weapons);
	TestTrue(TEXT("rebind"), Rebind.bBound);
	if (Rebind.bBound)
	{
		TestEqual(TEXT("identity unchanged"), Rebind.Record.InstanceId, Instance.InstanceId);
		TestEqual(TEXT("level unchanged"), Rebind.Record.Level, 4);
		TestEqual(TEXT("roll seed unchanged"), Rebind.Record.RollSeed, static_cast<int64>(90210));
		TestEqual(TEXT("rounds restored, not re-initialized"), Rebind.Record.LoadedRounds, 7);
	}
	return true;
}

/**
 * Every refusal names its reason and leaves the registry unchanged; there is
 * never a training-sword fallback for broken mappings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019RejectionsNameReasonNoFallback,
	"UEMMO.Tasks.M5_019.RejectionsNameReasonNoFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019RejectionsNameReasonNoFallback::RunTest(const FString& Parameters)
{
	FCombatCatalog Weapons;
	FString Error;
	if (!MakeCombatCatalog(Weapons, Error))
	{
		AddError(FString::Printf(TEXT("combat catalog build failed: %s"), *Error));
		return false;
	}
	const FItemDefinitionCatalog Items = MakeItemCatalog();
	FWeaponBindingRegistry Registry;

	auto ExpectReject = [&](const FItemInstance& Instance, EWeaponBindingRejectReason Expected, const TCHAR* Label)
	{
		const FWeaponBindOutcome Outcome = Registry.BindInstance(Instance, Items, Weapons);
		TestFalse(FString::Printf(TEXT("%s refused"), Label), Outcome.bBound);
		TestEqual(FString::Printf(TEXT("%s reason"), Label), Outcome.RejectReason, Expected);
		TestFalse(FString::Printf(TEXT("%s names a detail"), Label), Outcome.RejectDetail.IsEmpty());
	};

	// Invalid instance: all-zero guid and empty definition id.
	FItemInstance ZeroGuid = MakeInstance(ItemTrainingSword, 1);
	ZeroGuid.InstanceId = FGuid();
	ExpectReject(ZeroGuid, EWeaponBindingRejectReason::InvalidInstance, TEXT("zero guid"));

	FItemInstance NoDefinition = MakeInstance(ItemTrainingSword, 1);
	NoDefinition.DefinitionId = NAME_None;
	ExpectReject(NoDefinition, EWeaponBindingRejectReason::InvalidInstance, TEXT("empty definition id"));

	// Unknown item definition.
	ExpectReject(MakeInstance(TEXT("item_unknown"), 1), EWeaponBindingRejectReason::UnknownItemDefinition, TEXT("unknown item"));

	// Non-weapon slots: the M3 armor and charm never become weapons.
	ExpectReject(MakeInstance(ItemArmor, 1), EWeaponBindingRejectReason::SlotNotWeapon, TEXT("armor slot"));
	ExpectReject(MakeInstance(ItemCharm, 1), EWeaponBindingRejectReason::SlotNotWeapon, TEXT("accessory slot"));

	// Weapon slot without a mapping, and a mapping that resolves to nothing.
	ExpectReject(MakeInstance(ItemUnmapped, 1), EWeaponBindingRejectReason::NoWeaponMapping, TEXT("missing mapping"));
	ExpectReject(MakeInstance(ItemBadMapping, 1), EWeaponBindingRejectReason::UnknownWeaponDefinition, TEXT("unknown weapon"));

	// Duplicate active binding of the same instance.
	const FItemInstance Bound = MakeInstance(ItemTrainingSword, 5);
	TestTrue(TEXT("initial bind for duplicate check"), Registry.BindInstance(Bound, Items, Weapons).bBound);
	ExpectReject(Bound, EWeaponBindingRejectReason::DuplicateInstanceBinding, TEXT("duplicate binding"));

	// No training-sword fallback ever appeared: the failed attempts left
	// exactly one record (the intentional sword binding) and nothing else.
	TestEqual(TEXT("only the intentional binding exists"), Registry.NumBindings(), 1);
	const FWeaponBindingRecord* OnlyRecord = Registry.FindBinding(Bound.InstanceId);
	if (OnlyRecord == nullptr)
	{
		AddError(TEXT("the intentional binding must stay resolvable"));
		return false;
	}
	TestEqual(TEXT("no fallback weapon id"), OnlyRecord->WeaponDefinitionId, FName(TEXT("weapon_training_sword")));
	return true;
}

/**
 * The magazine slot moves only through the guarded setter: unbound instances,
 * melee weapons and out-of-range values are refused with a named error.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019SetInstanceRoundsGuarded,
	"UEMMO.Tasks.M5_019.SetInstanceRoundsGuarded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019SetInstanceRoundsGuarded::RunTest(const FString& Parameters)
{
	FCombatCatalog Weapons;
	FString Error;
	if (!MakeCombatCatalog(Weapons, Error))
	{
		AddError(FString::Printf(TEXT("combat catalog build failed: %s"), *Error));
		return false;
	}
	const FItemDefinitionCatalog Items = MakeItemCatalog();
	FWeaponBindingRegistry Registry;

	const FGuid UnboundId = FGuid::NewGuid();
	FString GuardError;
	TestEqual(TEXT("unbound instance refuses"), Registry.SetInstanceRounds(UnboundId, 3, &GuardError) ? 1 : 0, 0);
	TestFalse(TEXT("unbound error names the problem"), GuardError.IsEmpty());

	// Melee: a bound sword has no magazine to mutate.
	const FItemInstance SwordInstance = MakeInstance(ItemTrainingSword, 11);
	TestTrue(TEXT("sword binds"), Registry.BindInstance(SwordInstance, Items, Weapons).bBound);
	GuardError.Reset();
	TestEqual(TEXT("melee refuses"), Registry.SetInstanceRounds(SwordInstance.InstanceId, 1, &GuardError) ? 1 : 0, 0);
	TestFalse(TEXT("melee error names the problem"), GuardError.IsEmpty());

	// Ranged: bounds are enforced, valid values persist.
	const FItemInstance GunInstance = MakeInstance(ItemRangedTest, 12);
	TestTrue(TEXT("gun binds"), Registry.BindInstance(GunInstance, Items, Weapons).bBound);

	GuardError.Reset();
	TestEqual(TEXT("negative refuses"), Registry.SetInstanceRounds(GunInstance.InstanceId, -1, &GuardError) ? 1 : 0, 0);
	TestFalse(TEXT("negative error names the problem"), GuardError.IsEmpty());

	GuardError.Reset();
	TestEqual(TEXT("over capacity refuses"), Registry.SetInstanceRounds(GunInstance.InstanceId, 13, &GuardError) ? 1 : 0, 0);
	TestFalse(TEXT("over capacity error names the problem"), GuardError.IsEmpty());

	TestEqual(TEXT("zero is legal"), Registry.SetInstanceRounds(GunInstance.InstanceId, 0, nullptr) ? 1 : 0, 1);
	TestEqual(TEXT("full is legal"), Registry.SetInstanceRounds(GunInstance.InstanceId, 12, nullptr) ? 1 : 0, 1);
	TestEqual(TEXT("seven is legal"), Registry.SetInstanceRounds(GunInstance.InstanceId, 7, nullptr) ? 1 : 0, 1);

	const FWeaponBindingRecord* Record = Registry.FindBinding(GunInstance.InstanceId);
	if (Record == nullptr)
	{
		AddError(TEXT("gun binding must stay resolvable"));
		return false;
	}
	TestEqual(TEXT("value persisted"), Record->LoadedRounds, 7);
	return true;
}

/**
 * Session teardown: Reset drops every binding and parked magazine, so a fresh
 * session rebinds from a clean state (first bind full again).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019ResetClearsSessionState,
	"UEMMO.Tasks.M5_019.ResetClearsSessionState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019ResetClearsSessionState::RunTest(const FString& Parameters)
{
	FCombatCatalog Weapons;
	FString Error;
	if (!MakeCombatCatalog(Weapons, Error))
	{
		AddError(FString::Printf(TEXT("combat catalog build failed: %s"), *Error));
		return false;
	}
	const FItemDefinitionCatalog Items = MakeItemCatalog();
	FWeaponBindingRegistry Registry;

	const FItemInstance Instance = MakeInstance(ItemRangedTest, 21);
	TestTrue(TEXT("bind"), Registry.BindInstance(Instance, Items, Weapons).bBound);
	TestTrue(TEXT("spend"), Registry.SetInstanceRounds(Instance.InstanceId, 4));
	TestTrue(TEXT("unbind parks the magazine"), Registry.UnbindInstance(Instance.InstanceId));

	Registry.Reset();
	TestEqual(TEXT("bindings cleared"), Registry.NumBindings(), 0);

	// After a reset the parked state is gone: the next bind starts full again.
	const FWeaponBindOutcome FreshBind = Registry.BindInstance(Instance, Items, Weapons);
	TestTrue(TEXT("rebind after reset"), FreshBind.bBound);
	TestEqual(TEXT("fresh session starts full"), FreshBind.Record.LoadedRounds, 12);
	return true;
}

/**
 * The M5-019 ItemDefinition.h addition: the weapon mapping is only legal on a
 * Weapon-slot definition with a syntactically valid id; armor/charm without
 * the field validate exactly as before (M3 behavior unchanged).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_019ItemDefinitionMappingValidation,
	"UEMMO.Tasks.M5_019.ItemDefinitionMappingValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_019ItemDefinitionMappingValidation::RunTest(const FString& Parameters)
{
	// A weapon item with a valid mapping validates.
	FItemDefinition Sword;
	Sword.DefinitionId = TEXT("weapon_training");
	Sword.Slot = EItemSlot::Weapon;
	Sword.BaseStats.Attack = 5.0f;
	Sword.WeaponDefinitionId = TEXT("weapon_training_sword");
	FString Errors;
	TestTrue(TEXT("mapped weapon item validates"), ValidateItemDefinition(Sword, Errors));

	// Armor/charm without the field validate exactly as before.
	FItemDefinition Armor;
	Armor.DefinitionId = TEXT("armor_training");
	Armor.Slot = EItemSlot::Armor;
	Armor.BaseStats.Defense = 3.0f;
	TestTrue(TEXT("armor without mapping validates"), ValidateItemDefinition(Armor, Errors));

	FItemDefinition Charm;
	Charm.DefinitionId = TEXT("charm_training");
	Charm.Slot = EItemSlot::Accessory;
	Charm.BaseStats.MaxHP = 20.0f;
	TestTrue(TEXT("charm without mapping validates"), ValidateItemDefinition(Charm, Errors));

	// A non-weapon slot with a mapping is rejected and names the field.
	FItemDefinition ArmoredSword = Armor;
	ArmoredSword.WeaponDefinitionId = TEXT("weapon_training_sword");
	TestEqual(TEXT("armor with mapping refuses"), ValidateItemDefinition(ArmoredSword, Errors) ? 1 : 0, 0);
	TestTrue(TEXT("error names WeaponDefinitionId"), Errors.Contains(TEXT("WeaponDefinitionId")));

	// A weapon slot with a malformed id text is rejected.
	FItemDefinition BadText = Sword;
	BadText.WeaponDefinitionId = TEXT("Weapon-Training!");
	TestEqual(TEXT("malformed weapon id refuses"), ValidateItemDefinition(BadText, Errors) ? 1 : 0, 0);
	TestTrue(TEXT("error names WeaponDefinitionId"), Errors.Contains(TEXT("WeaponDefinitionId")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
