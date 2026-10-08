// M5-020: the production catalog mount and the equipment switch (M5 interface
// contract section 1 owner 019..025 "WeaponComponent", section 7 "存档与实例").
// Pins Weapons/WeaponComponent.h through the real pawn:
//
// - The mount accepts only a BUILT combat catalog (non-empty ConfigRevision);
//   a refused mount latches the explicit catalog-error state and every apply
//   refuses with its named reason - no fallback weapon, no fake success.
// - Bind resolves the equipped item through the M5-019 registry (identity
//   preserved verbatim) and mounts the M5-021 ammo model lazily (pool cap
//   from the ammo type with an empty initial reserve - supply belongs to the
//   later lineage; the binding record starts full per M5-019 while the live
//   magazine slot starts empty and only the reload/supply lineage fills it).
// - Switching tears the previous binding down FIRST: unbind parks the
//   magazine rounds, an open reload window closes without transferring, and
//   the binding generation bumps so stale fire/reload callbacks lose effect.
//   Rebinding the same instance restores the parked rounds and never adds
//   magazine or reserve ammunition.
// - A melee weapon without exactly the legacy four-attack set is refused with
//   a named reason (no silent ignoring of table fields); the X/Z chain itself
//   stays untouched (the legacy combat component keeps its own suites).
// - Death closes the reload window and voids pending callbacks; the binding
//   identity survives and the revive re-entry restores it.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../PrototypeCharacter.h"
#include "../Weapons/WeaponComponent.h"

#include "HAL/FileManager.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_020
{
	/**
	 * The combat catalog fixture (the 019 style): the legacy four-attack
	 * training sword, a 12-round hitscan row on ammo_cell, plus one BROKEN
	 * melee row (incomplete attack set) that a non-validating builder can
	 * carry - BuildFromParsed checks lookup integrity only, so the mount-level
	 * AttackSet refusal is the last line of defense.
	 */
	bool MakeCombatCatalog(FCombatCatalog& OutCatalog, FString& OutError)
	{
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;

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

		for (const FName AttackId : GetLegacyMeleeAttackIds())
		{
			FAttackReaction Row;
			Row.ReactionId = AttackId;
			Row.ControlPenetration = EControlPenetration::None;
			Parsed.AttackReactions.Add(Row);
		}

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

		FWeaponDefinition Broken;
		Broken.WeaponId = TEXT("weapon_broken_melee");
		Broken.FireMode = EWeaponFireMode::Melee;
		Broken.DamageProfileId = TEXT("light_01");
		Broken.MeleeAttackIds = { TEXT("light_01") };
		Parsed.Weapons.Add(Broken);

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
	const FName ItemBrokenMelee = TEXT("weapon_broken_melee_item");

	/** The item catalog fixture: mapped sword/ranged items plus a broken-mapped one. */
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

		FItemDefinition Broken;
		Broken.DefinitionId = ItemBrokenMelee;
		Broken.DisplayName = TEXT("Broken Melee");
		Broken.Slot = EItemSlot::Weapon;
		Broken.Rarity = EItemRarity::Normal;
		Broken.WeaponDefinitionId = TEXT("weapon_broken_melee");
		Catalog.AddDefinition(Broken);

		return Catalog;
	}

	/** Deterministic item instance (the M3 fixture style, M5-020 seeds). */
	FItemInstance MakeInstance(uint32 Seed, const FName DefinitionId)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x05200000u + Seed, 0x0BECu, Seed + 20u, Seed * 7u + 5u);
		Instance.DefinitionId = DefinitionId;
		Instance.RollSeed = static_cast<int64>(0x520 + Seed);
		Instance.Level = 1;
		return Instance;
	}
}

using namespace UE::UEMMO::Tasks::M5_020;

/**
 * Acceptance: only a built catalog mounts; an unbuilt one (empty revision) and
 * a null pair are refused with the explicit catalog-error latch, and every
 * apply against a disabled mount refuses with a named reason instead of a
 * fake success ("缺武器不是假成功").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020MountGate,
	"UEMMO.Tasks.M5_020.MountGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020MountGate::RunTest(const FString& Parameters)
{
	UWeaponComponent* Mount = NewObject<UWeaponComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("the weapon component instantiates standalone"), Mount))
	{
		return false;
	}

	// Unmounted: the apply refuses with the explicit catalog reason; nothing binds.
	FItemInstance SwordInstance = MakeInstance(1, ItemTrainingSword);
	const FWeaponMountOutcome Unmounted = Mount->ApplyEquippedWeapon(&SwordInstance);
	TestFalse(TEXT("an unmounted mount refuses the bind"), Unmounted.bSucceeded);
	TestEqual(TEXT("the unmounted refusal names the catalog"), static_cast<int32>(Unmounted.Reject),
		static_cast<int32>(EWeaponMountReject::CatalogUnavailable));
	TestFalse(TEXT("the unmounted mount binds nothing"), Unmounted.bWeaponBound);
	TestFalse(TEXT("the unmounted mount authorizes no fire"), Mount->IsFireAuthorized());
	TestTrue(TEXT("the unmounted mount reports no revision"), Mount->GetMountedRevision().IsEmpty());

	// A never-built catalog (empty revision) is refused and keeps the latch.
	FCombatCatalog Unbuilt;
	FItemDefinitionCatalog Items = MakeItemCatalog();
	FString MountError;
	TestFalse(TEXT("a never-built combat catalog is refused"), Mount->MountCatalogs(&Unbuilt, &Items, &MountError));
	TestFalse(TEXT("the mount error names the empty revision"), MountError.IsEmpty());
	TestTrue(TEXT("the refused mount latches the catalog error"), Mount->IsCatalogError());

	// The healthy pair mounts and publishes the revision.
	FCombatCatalog Catalog;
	FString BuildError;
	if (!TestTrue(TEXT("the fixture combat catalog builds"), MakeCombatCatalog(Catalog, BuildError)))
	{
		return false;
	}
	FString ExpectedRevision = Catalog.GetConfigRevision();
	TestTrue(TEXT("the built catalog mounts"), Mount->MountCatalogs(&Catalog, &Items, &MountError));
	TestFalse(TEXT("a successful mount clears the catalog error"), Mount->IsCatalogError());
	TestEqual(TEXT("the mounted revision is the catalog revision"), Mount->GetMountedRevision(), ExpectedRevision);
	TestTrue(TEXT("the fixture revision is non-empty"), !ExpectedRevision.IsEmpty());

	// A refused re-mount keeps the previously mounted pair intact.
	TestFalse(TEXT("re-mounting the unbuilt catalog is refused"), Mount->MountCatalogs(&Unbuilt, &Items, &MountError));
	TestEqual(TEXT("the healthy revision survives the refused re-mount"), Mount->GetMountedRevision(), ExpectedRevision);
	return true;
}

/**
 * Acceptance: the real pawn's mount binds the equipped item through the M5-019
 * chain - identity preserved verbatim, the resolved definition and revision are
 * the catalog's, melee authorizes nothing, a ranged bind opens fire.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020RealPawnBind,
	"UEMMO.Tasks.M5_020.RealPawnBind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020RealPawnBind::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
		Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World) ||
		!TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
	{
		return false;
	}
	FActorSpawnParameters Params;
	APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(), FVector(100.0, 0.0, 120.0), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("the real prototype character spawns"), Player))
	{
		return false;
	}
	UWeaponComponent* Mount = Player->GetWeaponMount();
	if (!TestNotNull(TEXT("the pawn carries the weapon mount"), Mount))
	{
		return false;
	}

	FCombatCatalog Catalog;
	FString BuildError;
	if (!TestTrue(TEXT("the fixture combat catalog builds"), MakeCombatCatalog(Catalog, BuildError)))
	{
		return false;
	}
	FItemDefinitionCatalog Items = MakeItemCatalog();
	TestTrue(TEXT("the fixture pair mounts"), Mount->MountCatalogs(&Catalog, &Items));
	const FString ExpectedRevision = Catalog.GetConfigRevision();

	// Melee bind: identity preserved verbatim, fire stays unauthorized.
	FItemInstance SwordInstance = MakeInstance(2, ItemTrainingSword);
	const FWeaponMountOutcome Melee = Mount->ApplyEquippedWeapon(&SwordInstance);
	TestTrue(TEXT("the training sword binds"), Melee.bSucceeded && Melee.bWeaponBound);
	TestTrue(TEXT("the melee bind reports no rejection"), Melee.Reject == EWeaponMountReject::None);
	const FWeaponBindingRecord* MeleeRecord = Mount->GetActiveBinding();
	TestNotNull(TEXT("the melee binding is active"), MeleeRecord);
	if (MeleeRecord != nullptr)
	{
		TestEqual(TEXT("the melee binding keeps the instance identity"), MeleeRecord->InstanceId, SwordInstance.InstanceId);
		TestEqual(TEXT("the melee binding resolves the mapped weapon"), MeleeRecord->WeaponDefinitionId, FName(TEXT("weapon_training_sword")));
		TestEqual(TEXT("the melee binding keeps the roll seed"), MeleeRecord->RollSeed, SwordInstance.RollSeed);
		TestEqual(TEXT("the melee binding keeps the level"), MeleeRecord->Level, SwordInstance.Level);
		TestEqual(TEXT("the melee magazine stays 0"), MeleeRecord->LoadedRounds, 0);
	}
	TestFalse(TEXT("melee never authorizes component fire"), Mount->IsFireAuthorized());

	// Ranged bind: the mount re-resolves and authorizes fire.
	FItemInstance RangedInstance = MakeInstance(3, ItemRangedTest);
	const FWeaponMountOutcome Ranged = Mount->ApplyEquippedWeapon(&RangedInstance);
	TestTrue(TEXT("the ranged test gun binds"), Ranged.bSucceeded && Ranged.bWeaponBound);
	const FWeaponBindingRecord* RangedRecord = Mount->GetActiveBinding();
	TestNotNull(TEXT("the ranged binding is active"), RangedRecord);
	if (RangedRecord != nullptr)
	{
		TestEqual(TEXT("the ranged binding resolves the mapped weapon"), RangedRecord->WeaponDefinitionId, FName(TEXT("weapon_hitscan_sample")));
		TestEqual(TEXT("the ranged magazine starts full"), RangedRecord->LoadedRounds, 12);
	}
	TestTrue(TEXT("the ranged bind authorizes fire"), Mount->IsFireAuthorized());
	TestEqual(TEXT("the mounted revision still matches the catalog"), Mount->GetMountedRevision(), ExpectedRevision);

	// The lazy ammo provisioning: pool cap from the ammo type, EMPTY initial
	// reserve (supply belongs to the later lineage) and an EMPTY live magazine
	// slot (rounds enter through the reload/supply lineage; the M5-019 binding
	// record keeps its own first-bind-full book).
	TestEqual(TEXT("the shared reserve pool registers empty"), Mount->GetAmmoModel().GetReserveRounds(FName(TEXT("ammo_cell"))), 0);
	const FMagazineState* Magazine = Mount->GetAmmoModel().FindMagazine(RangedInstance.InstanceId);
	TestNotNull(TEXT("the per-instance magazine initializes"), Magazine);
	if (Magazine != nullptr)
	{
		TestEqual(TEXT("the magazine capacity mirrors the weapon"), Magazine->Capacity, 12);
		TestEqual(TEXT("the live magazine starts empty"), Magazine->LoadedRounds, 0);
	}
	return true;
}

/**
 * Acceptance: the switch tears the previous binding down first - the open
 * reload window closes without transferring, the magazine rounds park and
 * restore on the rebind, and neither the magazine nor the shared reserve gains
 * ammunition ("同实例重绑弹匣/备弹不增").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020SwitchKeepsAmmoBooks,
	"UEMMO.Tasks.M5_020.SwitchKeepsAmmoBooks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020SwitchKeepsAmmoBooks::RunTest(const FString& Parameters)
{
	UWeaponComponent* Mount = NewObject<UWeaponComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("the weapon component instantiates standalone"), Mount))
	{
		return false;
	}
	FCombatCatalog Catalog;
	FString BuildError;
	if (!TestTrue(TEXT("the fixture combat catalog builds"), MakeCombatCatalog(Catalog, BuildError)))
	{
		return false;
	}
	FItemDefinitionCatalog Items = MakeItemCatalog();
	if (!TestTrue(TEXT("the fixture pair mounts"), Mount->MountCatalogs(&Catalog, &Items)))
	{
		return false;
	}

	// Pre-register the shared pool WITH stock: the mount must never add to it.
	FString AmmoError;
	if (!TestTrue(TEXT("the pre-registered pool holds 60 rounds"),
		Mount->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_cell")), 120, 60, &AmmoError)))
	{
		AddError(FString::Printf(TEXT("pool registration refused: %s"), *AmmoError));
		return false;
	}

	FItemInstance RangedInstance = MakeInstance(4, ItemRangedTest);
	if (!TestTrue(TEXT("the ranged weapon binds"), Mount->ApplyEquippedWeapon(&RangedInstance).bWeaponBound))
	{
		return false;
	}
	// Consume the registry slot down to 5 (the guarded M5-019 write).
	FString RoundsError;
	if (!TestTrue(TEXT("the registry slot drops to 5"),
		Mount->GetRegistry().SetInstanceRounds(RangedInstance.InstanceId, 5, &RoundsError)))
	{
		AddError(FString::Printf(TEXT("SetInstanceRounds refused: %s"), *RoundsError));
		return false;
	}

	// Open a reload window, then switch: the window closes, nothing moves.
	const FAmmoReloadOutcome Window = Mount->GetAmmoModel().BeginReload(RangedInstance.InstanceId, 100.0, 5.0);
	if (!TestTrue(TEXT("the reload window opens"), Window.bSuccess))
	{
		AddError(FString::Printf(TEXT("BeginReload refused: %s"), *Window.ErrorDetail));
		return false;
	}
	FItemInstance SwordInstance = MakeInstance(5, ItemTrainingSword);
	const FWeaponMountOutcome Switch = Mount->ApplyEquippedWeapon(&SwordInstance);
	TestTrue(TEXT("the switch to the sword succeeds"), Switch.bSucceeded && Switch.bWeaponBound);
	TestFalse(TEXT("the switch closes the old reload window"), Mount->GetAmmoModel().IsReloading(RangedInstance.InstanceId));
	TestEqual(TEXT("the closed window transferred nothing"), Mount->GetAmmoModel().GetReserveRounds(FName(TEXT("ammo_cell"))), 60);
	TestFalse(TEXT("the old binding is gone"), Mount->GetRegistry().FindBinding(RangedInstance.InstanceId) != nullptr);
	const FMagazineState* ParkedMagazine = Mount->GetAmmoModel().FindMagazine(RangedInstance.InstanceId);
	TestTrue(TEXT("the magazine state stays parked for the rebind"), ParkedMagazine != nullptr);

	// Rebind the same instance: rounds restore, nothing increases.
	const FWeaponMountOutcome Rebind = Mount->ApplyEquippedWeapon(&RangedInstance);
	TestTrue(TEXT("the rebind succeeds"), Rebind.bSucceeded && Rebind.bWeaponBound);
	if (Rebind.bWeaponBound)
	{
		TestEqual(TEXT("the parked registry rounds restore"), Rebind.Record.LoadedRounds, 5);
	}
	const FMagazineState* Restored = Mount->GetAmmoModel().FindMagazine(RangedInstance.InstanceId);
	TestNotNull(TEXT("the model magazine survives the rebind"), Restored);
	if (Restored != nullptr)
	{
		TestEqual(TEXT("the model magazine never re-initializes"), Restored->LoadedRounds, 0);
	}
	TestEqual(TEXT("the shared reserve never grows on a rebind"), Mount->GetAmmoModel().GetReserveRounds(FName(TEXT("ammo_cell"))), 60);

	// The explicit no-weapon apply (empty slot): unbound, fire disabled.
	const FWeaponMountOutcome Unequip = Mount->ApplyEquippedWeapon(nullptr);
	TestTrue(TEXT("the explicit no-weapon apply succeeds"), Unequip.bSucceeded);
	TestFalse(TEXT("the no-weapon apply binds nothing"), Unequip.bWeaponBound);
	TestFalse(TEXT("the no-weapon mount authorizes no fire"), Mount->IsFireAuthorized());
	return true;
}

/**
 * Acceptance: every teardown bumps the binding generation - stale callbacks
 * captured before a switch or a death report not-current and must drop their
 * work; death additionally closes the reload window and disables fire while
 * the binding identity survives for the revive re-entry.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020StaleCallbacksAndDeath,
	"UEMMO.Tasks.M5_020.StaleCallbacksAndDeath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020StaleCallbacksAndDeath::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World) ||
		!TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
	{
		return false;
	}
	FActorSpawnParameters Params;
	APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(), FVector(100.0, 0.0, 120.0), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("the real prototype character spawns"), Player))
	{
		return false;
	}
	UWeaponComponent* Mount = Player->GetWeaponMount();
	if (!TestNotNull(TEXT("the pawn carries the weapon mount"), Mount))
	{
		return false;
	}
	FCombatCatalog Catalog;
	FString BuildError;
	if (!TestTrue(TEXT("the fixture combat catalog builds"), MakeCombatCatalog(Catalog, BuildError)))
	{
		return false;
	}
	FItemDefinitionCatalog Items = MakeItemCatalog();
	if (!TestTrue(TEXT("the fixture pair mounts"), Mount->MountCatalogs(&Catalog, &Items)))
	{
		return false;
	}
	if (!TestTrue(TEXT("the pre-registered pool holds 60 rounds"),
		Mount->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_cell")), 120, 60)))
	{
		return false;
	}

	FItemInstance RangedInstance = MakeInstance(6, ItemRangedTest);
	if (!TestTrue(TEXT("the ranged weapon binds"), Mount->ApplyEquippedWeapon(&RangedInstance).bWeaponBound))
	{
		return false;
	}
	if (!TestTrue(TEXT("the reload window opens"), Mount->GetAmmoModel().BeginReload(RangedInstance.InstanceId, 50.0, 10.0).bSuccess))
	{
		return false;
	}

	// A switch: the captured generation goes stale, the window closes.
	const uint64 GenerationBefore = Mount->GetBindingGeneration();
	FItemInstance SwordInstance = MakeInstance(7, ItemTrainingSword);
	TestTrue(TEXT("the switch succeeds"), Mount->ApplyEquippedWeapon(&SwordInstance).bWeaponBound);
	TestFalse(TEXT("the pre-switch generation is stale"), Mount->IsBindingGenerationCurrent(GenerationBefore));
	TestTrue(TEXT("the current generation reports current"), Mount->IsBindingGenerationCurrent(Mount->GetBindingGeneration()));
	TestFalse(TEXT("the switch cancelled the old reload"), Mount->GetAmmoModel().IsReloading(RangedInstance.InstanceId));

	// Death through the real health lifecycle: window closes, fire disables,
	// binding identity survives.
	FItemInstance Rebound = RangedInstance;
	if (!TestTrue(TEXT("the ranged weapon rebinds"), Mount->ApplyEquippedWeapon(&Rebound).bWeaponBound))
	{
		return false;
	}
	if (!TestTrue(TEXT("the second reload window opens"), Mount->GetAmmoModel().BeginReload(Rebound.InstanceId, 200.0, 10.0).bSuccess))
	{
		return false;
	}
	const uint64 GenerationBeforeDeath = Mount->GetBindingGeneration();
	TestTrue(TEXT("the lethal damage kills the pawn"), Player->GetHealth()->ApplyDamage(1000.0f) > 0.0f);
	TestTrue(TEXT("death bumps the generation"), !Mount->IsBindingGenerationCurrent(GenerationBeforeDeath));
	TestFalse(TEXT("death closed the reload window"), Mount->GetAmmoModel().IsReloading(Rebound.InstanceId));
	TestFalse(TEXT("death disables fire"), Mount->IsFireAuthorized());
	TestNotNull(TEXT("death keeps the binding identity"), Mount->GetActiveBinding());
	TestEqual(TEXT("the shared reserve survived death untouched"), Mount->GetAmmoModel().GetReserveRounds(FName(TEXT("ammo_cell"))), 60);

	// Revive re-entry: alive again; the re-applied load restores the binding.
	Mount->NotifyOwnerRevived();
	if (!TestTrue(TEXT("the revive rebind restores fire"), Mount->ApplyEquippedWeapon(&Rebound).bWeaponBound))
	{
		return false;
	}
	TestTrue(TEXT("the revived ranged mount authorizes fire again"), Mount->IsFireAuthorized());
	return true;
}

/**
 * Acceptance: a melee weapon whose attack set is not exactly the legacy
 * four-attack set is refused with a named reason, leaves NO half-bound state
 * and never silently ignores the table fields ("配置不支持的AttackSet明确拒绝").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020UnsupportedMeleeAttackSet,
	"UEMMO.Tasks.M5_020.UnsupportedMeleeAttackSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020UnsupportedMeleeAttackSet::RunTest(const FString& Parameters)
{
	UWeaponComponent* Mount = NewObject<UWeaponComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("the weapon component instantiates standalone"), Mount))
	{
		return false;
	}
	FCombatCatalog Catalog;
	FString BuildError;
	if (!TestTrue(TEXT("the fixture combat catalog builds (the broken row enters a non-validating build)"),
		MakeCombatCatalog(Catalog, BuildError)))
	{
		return false;
	}
	FItemDefinitionCatalog Items = MakeItemCatalog();
	if (!TestTrue(TEXT("the fixture pair mounts"), Mount->MountCatalogs(&Catalog, &Items)))
	{
		return false;
	}

	FItemInstance BrokenInstance = MakeInstance(8, ItemBrokenMelee);
	const FWeaponMountOutcome Refused = Mount->ApplyEquippedWeapon(&BrokenInstance);
	TestFalse(TEXT("the broken attack set refuses the bind"), Refused.bSucceeded);
	TestEqual(TEXT("the refusal names the attack set"), static_cast<int32>(Refused.Reject),
		static_cast<int32>(EWeaponMountReject::UnsupportedMeleeAttackSet));
	TestFalse(TEXT("the refusal detail is named"), Refused.RejectDetail.IsEmpty());
	TestFalse(TEXT("no binding stays active after the refusal"), Mount->GetActiveBinding() != nullptr);
	TestEqual(TEXT("the registry holds no half binding"), Mount->GetRegistry().NumBindings(), 0);
	TestFalse(TEXT("the refused mount authorizes no fire"), Mount->IsFireAuthorized());

	// The mount stays usable: the healthy sword still binds afterwards.
	FItemInstance SwordInstance = MakeInstance(9, ItemTrainingSword);
	const FWeaponMountOutcome Recovery = Mount->ApplyEquippedWeapon(&SwordInstance);
	TestTrue(TEXT("the healthy sword binds after the refusal"), Recovery.bSucceeded && Recovery.bWeaponBound);
	return true;
}

/**
 * Acceptance: the bare-world BeginPlay (no HUD, no profile) keeps the mount
 * unbound and fire disabled - the documented graceful degradation, never a
 * fake bind. The refresh entry reports the skip explicitly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020BareWorldStaysUnbound,
	"UEMMO.Tasks.M5_020.BareWorldStaysUnbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020BareWorldStaysUnbound::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World) ||
		!TestTrue(TEXT("play begins in the test world (BeginPlay ran the mount refresh)"), Wrapper.BeginPlayInTestWorld()))
	{
		return false;
	}
	FActorSpawnParameters Params;
	APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(), FVector(100.0, 0.0, 120.0), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("the real prototype character spawns"), Player))
	{
		return false;
	}
	UWeaponComponent* Mount = Player->GetWeaponMount();
	if (!TestNotNull(TEXT("the pawn carries the weapon mount"), Mount))
	{
		return false;
	}
	TestFalse(TEXT("the bare-world BeginPlay bound nothing"), Mount->GetActiveBinding() != nullptr);
	TestFalse(TEXT("the bare-world mount authorizes no fire"), Mount->IsFireAuthorized());
	TestFalse(TEXT("the refresh entry reports the skip"), Player->RefreshWeaponMountFromEquipment());
	TestFalse(TEXT("the skipped refresh still binds nothing"), Mount->GetActiveBinding() != nullptr);
	return true;
}

/**
 * Acceptance: the production recipe the pawn's EnsureWeaponCatalogsMounted
 * uses - the six Data/CombatSystem tables through the M5-005 loader into the
 * M5-007 builder - mounts with a real revision, and the production items
 * source loads into the item catalog (both conditional on the dev-tree
 * sources; a packaged build skips with a warning and keeps firing disabled).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_020ProductionSourceMount,
	"UEMMO.Tasks.M5_020.ProductionSourceMount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_020ProductionSourceMount::RunTest(const FString& Parameters)
{
	const FString CombatDir = FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem");
	const FString ItemsPath = FPaths::ProjectDir() / TEXT("Data") / TEXT("items.json");
	if (!IFileManager::Get().DirectoryExists(*CombatDir) || !IFileManager::Get().FileExists(*ItemsPath))
	{
		AddWarning(TEXT("the production sources are absent (packaged tree); the mount gate keeps firing disabled"));
		return true;
	}

	UWeaponComponent* Mount = NewObject<UWeaponComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("the weapon component instantiates standalone"), Mount))
	{
		return false;
	}
	FCombatCatalog Catalog;
	FString BuildError;
	FString ExpectedRevision;
	{
		// The exact recipe of APrototypeCharacter::EnsureWeaponCatalogsMounted.
		FCombatDataTableSet Tables;
		TArray<FString> LoadProblems;
		if (!TestTrue(TEXT("the shipped combat tables load"), LoadCombatDataDirectory(CombatDir, Tables, LoadProblems)))
		{
			AddError(FString::Printf(TEXT("load problems: %s"), *FString::Join(LoadProblems, TEXT(" | "))));
			return false;
		}
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;
		for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles)
		{
			Parsed.DamageProfiles.Add(Row.Value);
		}
		for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions)
		{
			Parsed.AttackReactions.Add(Row.Value);
		}
		for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions)
		{
			Parsed.TargetReactions.Add(Row.Value);
		}
		for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons)
		{
			Parsed.Weapons.Add(Row.Value);
		}
		for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes)
		{
			Parsed.AmmoTypes.Add(Row.Value);
		}
		for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles)
		{
			Parsed.Projectiles.Add(Row.Value);
		}
		if (!TestTrue(TEXT("the production catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, Catalog, BuildError)))
		{
			AddError(FString::Printf(TEXT("build errors: %s"), *BuildError));
			return false;
		}
		ExpectedRevision = Catalog.GetConfigRevision();
	}
	TestTrue(TEXT("the production revision is non-empty"), !ExpectedRevision.IsEmpty());

	FItemDefinitionCatalog Items;
	{
		TArray<FTestRoomItemRow> ItemRows;
		FString SourceError;
		if (!TestTrue(TEXT("the production items load"), ACombatTestRoomDriver::LoadProductionItems(ItemRows, SourceError)))
		{
			AddError(FString::Printf(TEXT("items source error: %s"), *SourceError));
			return false;
		}
		TestTrue(TEXT("the production items are non-empty"), ItemRows.Num() > 0);
		for (const FTestRoomItemRow& Row : ItemRows)
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
			FString DefinitionError;
			if (!TestTrue(TEXT("the production item definition is accepted"), Items.AddDefinition(Definition, &DefinitionError)))
			{
				AddError(FString::Printf(TEXT("item '%s' refused: %s"), *Row.DefinitionId.ToString(), *DefinitionError));
				return false;
			}
		}
	}

	if (!TestTrue(TEXT("the production pair mounts"), Mount->MountCatalogs(&Catalog, &Items)))
	{
		return false;
	}
	TestEqual(TEXT("the mounted revision is the production revision"), Mount->GetMountedRevision(), ExpectedRevision);
	return true;
}

#endif
