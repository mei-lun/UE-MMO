// M5-022: the single-fire transaction and the atomic round deduction (M5
// interface contract section 3, owner 022 "WeaponComponent scheduling").
// Pins Weapons/WeaponComponent.h + Weapons/FirePolicies/SingleFirePolicy.h
// through the standalone mount (no World needed - every collaborator is a
// plain value type and the fire clock is injected):
//
// - The ordered gate chain refuses with named reasons and ZERO side effects:
//   unbound / melee / dead / menu / hit-stun / reloading / cooldown /
//   in-flight / stale-duplicate intent / empty magazine leave no deduction,
//   no sequence and no cooldown ("准入失败或生成预留失败不扣弹/不增sequence/不冷却").
// - One trigger commit is one common ActionSequence: the ShotId values are
//   monotonic fire-registry allocations (never the caller's local counter),
//   firing the LAST round still consumes it ("一发一Shot，打空仍消耗").
// - Duplicate intents and reentrant TryFire calls never double-fire
//   ("重复意图不多发"); death voids the in-flight shot and after the revive
//   the mount fires legally again ("回调重入、空弹和死亡无发射，恢复后可合法开火").
// - A policy reservation refusal and a sequence-allocation refusal are atomic:
//   the refusing-policy mount continues its sequence undisturbed; the
//   unregistered-source attempt rolls the deducted round back.
// - The component-level reload wrappers refill the fire book through the
//   per-cycle model slots - the reload ledger (M5-021) and the fire book
//   (M5-019) meet exactly here, and repeated refill cycles work.

#include "Misc/AutomationTest.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Weapons/WeaponComponent.h"
#include "../Weapons/FirePolicies/SingleFirePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_022
{
	// The M5-020 fixture recipe, mirrored per-file (each suite file owns its
	// fixtures): the legacy four-attack sword, the 12-round hitscan row on
	// ammo_cell (240 rpm) and the mapped item instances.
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

	/** The item catalog fixture: the mapped sword and ranged items. */
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

		return Catalog;
	}

	/** Deterministic item instance (the M3 fixture style). */
	FItemInstance MakeInstance(uint32 Seed, const FName DefinitionId)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x05220000u + Seed, 0x0BECu, Seed + 20u, Seed * 7u + 5u);
		Instance.DefinitionId = DefinitionId;
		Instance.RollSeed = static_cast<int64>(0x522 + Seed);
		Instance.Level = 1;
		return Instance;
	}

	/** A policy that refuses every reservation (the atomicity probe). */
	class FRefusingTestPolicy : public IFirePolicy
	{
	public:
		FCapacityReservation ReserveShotCapacity(const FShotContext& Candidate) override
		{
			FCapacityReservation Refusal;
			Refusal.RejectDetail = TEXT("test policy refuses every reservation");
			return Refusal;
		}
		void CommitShot(const FShotContext& Committed) override {}
		void AbortReservation(const FShotContext& Aborted) override {}
		void NotifyShotReleased(const FShotContext& Ended) override {}
	};

	/**
	 * The ready-to-fire rig: a standalone mount, the fixture pair, a ranged
	 * bind (registry fire book starts full at 12), a registered fire source
	 * and a manually advanced fire clock.
	 */
	struct FFireRig
	{
		UWeaponComponent* Mount = nullptr;
		FCombatCatalog Catalog;
		FItemDefinitionCatalog Items;
		FItemInstance RangedInstance;
		double Now = 100.0;

		bool Build(FString& OutError)
		{
			Mount = NewObject<UWeaponComponent>(GetTransientPackage());
			if (Mount == nullptr || !MakeCombatCatalog(Catalog, OutError))
			{
				return false;
			}
			Items = MakeItemCatalog();
			if (!Mount->MountCatalogs(&Catalog, &Items))
			{
				OutError = TEXT("the fixture pair refused the mount");
				return false;
			}
			if (!Mount->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_cell")), 120, 60))
			{
				OutError = TEXT("the shared pool registration refused");
				return false;
			}
			RangedInstance = MakeInstance(0x22, ItemRangedTest);
			const FWeaponMountOutcome Bind = Mount->ApplyEquippedWeapon(&RangedInstance);
			if (!Bind.bWeaponBound)
			{
				OutError = FString::Printf(TEXT("the ranged bind refused (%d): %s"),
					static_cast<int32>(Bind.Reject), *Bind.RejectDetail);
				return false;
			}
			FCombatEntityMetadata Meta;
			Meta.Faction = TEXT("player");
			Meta.Category = TEXT("player");
			if (!IsValidCombatEntityId(Mount->RegisterFireSource(Meta)))
			{
				OutError = TEXT("the fire source registration refused");
				return false;
			}
			Mount->SetFireClockProvider([this]() { return Now; });
			return true;
		}

		FFireOutcome Fire(uint64 LocalSequence)
		{
			FFireIntent Intent;
			Intent.WeaponInstanceId = RangedInstance.InstanceId;
			Intent.LocalShotSequence = LocalSequence;
			return Mount->TryFire(Intent);
		}

		/** The fire book (the M5-019 slot) of the active binding. */
		int32 Rounds() const
		{
			const FWeaponBindingRecord* Binding = Mount->GetActiveBinding();
			return Binding != nullptr ? Binding->LoadedRounds : -1;
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_022;

/**
 * Acceptance: every gate refusal is named and side-effect free - no round
 * deducted, no sequence allocated, no cooldown started ("准入失败不扣弹/
 * 不增sequence/不冷却").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_022FireGatesRefuseWithoutSideEffects,
	"UEMMO.Tasks.M5_022.FireGatesRefuseWithoutSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_022FireGatesRefuseWithoutSideEffects::RunTest(const FString& Parameters)
{
	// An unbound mount refuses with the named reason and no side effects.
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// Melee bound: the component fire path never fires for melee.
	FItemInstance SwordInstance = MakeInstance(0x23, ItemTrainingSword);
	TestTrue(TEXT("the melee sword binds"), Rig.Mount->ApplyEquippedWeapon(&SwordInstance).bWeaponBound);
	const FFireOutcome MeleeFire = Rig.Fire(1);
	TestEqual(TEXT("melee fire refuses as no-weapon"), static_cast<int32>(MeleeFire.Reject),
		static_cast<int32>(EFireReject::NoWeaponBound));
	TestFalse(TEXT("the melee refusal fires nothing"), MeleeFire.bFired);
	TestEqual(TEXT("the melee refusal holds no shot count"), Rig.Mount->GetCommittedShotCount(), 0);

	// The ranged bind restores the fire path (registry book full at 12).
	FItemInstance RangedInstance = MakeInstance(0x24, ItemRangedTest);
	TestTrue(TEXT("the ranged weapon rebinds"), Rig.Mount->ApplyEquippedWeapon(&RangedInstance).bWeaponBound);
	Rig.RangedInstance = RangedInstance;

	// Menu gate: refused, named, unchanged.
	Rig.Mount->SetMenuOpenPredicate([]() { return true; });
	const FFireOutcome MenuFire = Rig.Fire(1);
	TestEqual(TEXT("the open menu refuses the fire"), static_cast<int32>(MenuFire.Reject),
		static_cast<int32>(EFireReject::MenuOpen));
	Rig.Mount->SetMenuOpenPredicate(nullptr);

	// Hit-stun gate: refused, named, unchanged.
	Rig.Mount->SetHitStunPredicate([]() { return true; });
	const FFireOutcome StunFire = Rig.Fire(1);
	TestEqual(TEXT("the running stun refuses the fire"), static_cast<int32>(StunFire.Reject),
		static_cast<int32>(EFireReject::HitStunned));
	Rig.Mount->SetHitStunPredicate(nullptr);

	// Reload gate: a full fire book refuses the reload first (nothing to fill);
	// after one shot the window opens and fire refuses while it stays open.
	const FAmmoReloadOutcome FullBookReload = Rig.Mount->BeginReload(Rig.Now, 5.0);
	TestEqual(TEXT("a full fire book refuses the reload"), static_cast<int32>(FullBookReload.Error),
		static_cast<int32>(EAmmoModelError::MagazineFull));
	const FFireOutcome FirstShot = Rig.Fire(1);
	TestTrue(TEXT("the first shot commits"), FirstShot.bFired);
	Rig.Mount->ReleaseFire();
	Rig.Now += 1.0;
	const FAmmoReloadOutcome Window = Rig.Mount->BeginReload(Rig.Now, 5.0);
	TestTrue(TEXT("the reload window opens on a drained book"), Window.bSuccess);
	const FFireOutcome ReloadFire = Rig.Fire(2);
	TestEqual(TEXT("the open window refuses the fire"), static_cast<int32>(ReloadFire.Reject),
		static_cast<int32>(EFireReject::Reloading));
	Rig.Mount->CancelReload(FName(TEXT("test-cancel")));
	const FFireOutcome AfterCancel = Rig.Fire(2);
	TestTrue(TEXT("the cancelled window releases the gate"), AfterCancel.bFired);

	// Cooldown gate: the next pull inside the pacing window refuses.
	Rig.Mount->ReleaseFire();
	const FFireOutcome CooldownFire = Rig.Fire(3);
	TestEqual(TEXT("the pacing window refuses the fire"), static_cast<int32>(CooldownFire.Reject),
		static_cast<int32>(EFireReject::CooldownActive));

	// The gate refusals moved nothing beyond the two legal commits (shots 1
	// and 2): no extra deduction, no extra sequence, no extra cooldown.
	TestEqual(TEXT("only the legal commits counted"), Rig.Mount->GetCommittedShotCount(), 2);
	TestEqual(TEXT("only the legal commits deducted"), Rig.Rounds(), 10);
	FShotContext LastShot;
	TestTrue(TEXT("the last shot snapshot is the second commit"), Rig.Mount->GetLastShotContext(LastShot));
	TestEqual(TEXT("the sequence never advanced past the legal commits"), LastShot.ShotId, static_cast<FShotId>(2));
	TestTrue(TEXT("the cooldown start survived the refusals"),
		FMath::IsNearlyEqual(Rig.Mount->GetNextFireTimeSeconds(), 101.25, 1.0e-6));
	return true;
}

/**
 * Acceptance: one trigger is one common ActionSequence - the ShotId values
 * are monotonic fire-registry allocations, firing the last round still
 * consumes it and an empty magazine refuses with zero side effects
 * ("一发一Shot，打空仍消耗").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_022OneShotOneSequenceSpendToEmpty,
	"UEMMO.Tasks.M5_022.OneShotOneSequenceSpendToEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_022OneShotOneSequenceSpendToEmpty::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	for (int32 Index = 1; Index <= 12; ++Index)
	{
		const FFireOutcome Shot = Rig.Fire(static_cast<uint64>(Index));
		if (!TestTrue(FString::Printf(TEXT("shot %d commits"), Index), Shot.bFired))
		{
			return false;
		}
		TestEqual(FString::Printf(TEXT("shot %d takes the next common sequence"), Index),
			Shot.Shot.ShotId, static_cast<FShotId>(Index));
		TestEqual(FString::Printf(TEXT("shot %d keeps the local sequence as association"), Index),
			Shot.Shot.LocalShotSequence, static_cast<uint64>(Index));
		TestEqual(FString::Printf(TEXT("shot %d names the fire source"), Index),
			Shot.Shot.SourceEntityId, Rig.Mount->GetFireSourceEntityId());
		TestEqual(FString::Printf(TEXT("shot %d resolves the weapon"), Index),
			Shot.Shot.WeaponDefinitionId, FName(TEXT("weapon_hitscan_sample")));
		TestEqual(FString::Printf(TEXT("shot %d names the ammo"), Index),
			Shot.Shot.AmmoId, FName(TEXT("ammo_cell")));
		TestEqual(FString::Printf(TEXT("shot %d names the damage profile"), Index),
			Shot.Shot.DamageProfileId, FName(TEXT("light_02")));
		TestEqual(FString::Printf(TEXT("shot %d is hitscan"), Index),
			static_cast<int32>(Shot.Shot.FireMode), static_cast<int32>(EWeaponFireMode::Hitscan));
		TestEqual(FString::Printf(TEXT("shot %d carries the pellet count"), Index),
			Shot.Shot.PelletCount, 1);
		TestEqual(FString::Printf(TEXT("shot %d consumed one round"), Index), Rig.Rounds(), 12 - Index);
		TestEqual(FString::Printf(TEXT("shot %d counted"), Index), Rig.Mount->GetCommittedShotCount(), Index);
		Rig.Mount->ReleaseFire();
		Rig.Now += 1.0;
	}

	// The twelfth shot fired with the last round and emptied the book
	// ("打空仍消耗"): the thirteenth pull refuses, named and unchanged.
	TestEqual(TEXT("the book is empty"), Rig.Rounds(), 0);
	const FFireOutcome EmptyFire = Rig.Fire(13);
	TestEqual(TEXT("the empty book refuses the fire"), static_cast<int32>(EmptyFire.Reject),
		static_cast<int32>(EFireReject::MagazineEmpty));
	TestEqual(TEXT("the empty refusal committed nothing"), Rig.Mount->GetCommittedShotCount(), 12);
	FShotContext LastShot;
	TestTrue(TEXT("the last shot is still the twelfth"), Rig.Mount->GetLastShotContext(LastShot));
	TestEqual(TEXT("the sequence never advanced past the twelfth"), LastShot.ShotId, static_cast<FShotId>(12));
	return true;
}

/**
 * Acceptance: duplicate intents and reentrant fire calls never double-fire
 * ("重复意图不多发"); a repeated local sequence and a stale intent identity are
 * named refusals.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_022ReentrancyDuplicateAndInFlight,
	"UEMMO.Tasks.M5_022.ReentrancyDuplicateAndInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_022ReentrancyDuplicateAndInFlight::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// Reentrancy: the shot-committed handler re-fires after pushing the clock
	// past the fresh cooldown - the inner call must see the in-flight shot and
	// refuse, so the broadcast can never mint a second bullet.
	int32 ReentrantCalls = 0;
	Rig.Mount->OnShotCommitted.AddLambda([&Rig, &ReentrantCalls](const FShotContext& Shot)
	{
		++ReentrantCalls;
		Rig.Now += 1.0;
		Rig.Fire(Shot.LocalShotSequence + 100);
	});

	const FFireOutcome FirstShot = Rig.Fire(1);
	TestTrue(TEXT("the outer shot commits"), FirstShot.bFired);
	TestEqual(TEXT("the committed handler ran exactly once"), ReentrantCalls, 1);
	TestEqual(TEXT("the reentrant pull fired nothing"), Rig.Mount->GetCommittedShotCount(), 1);
	TestEqual(TEXT("the reentrant pull deducted nothing"), Rig.Rounds(), 11);
	Rig.Mount->OnShotCommitted.Clear();

	// In-flight: past the cooldown but before ReleaseFire the overlap refuses.
	Rig.Now += 1.0;
	const FFireOutcome InFlight = Rig.Fire(2);
	TestEqual(TEXT("the in-flight shot refuses the overlap"), static_cast<int32>(InFlight.Reject),
		static_cast<int32>(EFireReject::ShotInProgress));

	// After the release the SAME sequence is the named duplicate refusal.
	Rig.Mount->ReleaseFire();
	const FFireOutcome Duplicate = Rig.Fire(1);
	TestEqual(TEXT("the repeated sequence is a duplicate"), static_cast<int32>(Duplicate.Reject),
		static_cast<int32>(EFireReject::DuplicateIntent));

	// Stale intents: sequence 0 and a foreign instance identity are refused.
	const FFireOutcome ZeroSequence = Rig.Fire(0);
	TestEqual(TEXT("sequence 0 is a stale intent"), static_cast<int32>(ZeroSequence.Reject),
		static_cast<int32>(EFireReject::StaleIntent));
	FFireIntent Foreign;
	Foreign.WeaponInstanceId = MakeInstance(0x25, ItemRangedTest).InstanceId;
	Foreign.LocalShotSequence = 2;
	const FFireOutcome ForeignFire = Rig.Mount->TryFire(Foreign);
	TestEqual(TEXT("a foreign instance identity is a stale intent"), static_cast<int32>(ForeignFire.Reject),
		static_cast<int32>(EFireReject::StaleIntent));

	// The next legal sequence fires; nothing above minted a bullet or a round.
	Rig.Now += 1.0;
	const FFireOutcome SecondShot = Rig.Fire(2);
	TestTrue(TEXT("the next sequence fires legally"), SecondShot.bFired);
	TestEqual(TEXT("exactly two shots committed"), Rig.Mount->GetCommittedShotCount(), 2);
	TestEqual(TEXT("exactly two rounds left"), Rig.Rounds(), 10);
	return true;
}

/**
 * Acceptance: the reservation and the sequence allocation are atomic - a
 * refusing policy and an unallocated sequence both leave the round book
 * untouched (the post-deduction allocation failure rolls the round back) and
 * the next legal shot continues the sequence without a gap.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_022AtomicReservationAndSequence,
	"UEMMO.Tasks.M5_022.AtomicReservationAndSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_022AtomicReservationAndSequence::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// The refusing policy: the reservation refusal is named and side-effect free.
	FRefusingTestPolicy Refuser;
	Rig.Mount->SetFirePolicy(&Refuser);
	const FFireOutcome Reserved = Rig.Fire(1);
	TestEqual(TEXT("the refusing policy names the reservation"), static_cast<int32>(Reserved.Reject),
		static_cast<int32>(EFireReject::ReservationFailed));
	TestFalse(TEXT("the reservation refusal has no detail gap"), Reserved.RejectDetail.IsEmpty());
	TestEqual(TEXT("the reservation refusal deducted nothing"), Rig.Rounds(), 12);
	TestEqual(TEXT("the reservation refusal minted no sequence"), Rig.Mount->GetCommittedShotCount(), 0);
	FShotContext NoShot;
	TestFalse(TEXT("the reservation refusal left no shot snapshot"), Rig.Mount->GetLastShotContext(NoShot));

	// Restoring the default policy: the first legal shot takes sequence 1 -
	// the refused attempt never consumed an allocation.
	Rig.Mount->SetFirePolicy(nullptr);
	const FFireOutcome AfterRefusal = Rig.Fire(1);
	TestTrue(TEXT("the shot after the refusal commits"), AfterRefusal.bFired);
	TestEqual(TEXT("the sequence starts clean at 1"), AfterRefusal.Shot.ShotId, static_cast<FShotId>(1));
	TestEqual(TEXT("the shot after the refusal deducted once"), Rig.Rounds(), 11);

	// The unregistered fire source: the allocation fails AFTER the deduction,
	// so the round must roll back (the atomicity proof).
	UWeaponComponent* Orphan = NewObject<UWeaponComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("the orphan mount instantiates"), Orphan))
	{
		return false;
	}
	FCombatCatalog Catalog;
	FString BuildError;
	if (!TestTrue(TEXT("the fixture catalog builds"), MakeCombatCatalog(Catalog, BuildError)))
	{
		return false;
	}
	FItemDefinitionCatalog Items = MakeItemCatalog();
	TestTrue(TEXT("the orphan mounts the pair"), Orphan->MountCatalogs(&Catalog, &Items));
	TestTrue(TEXT("the orphan pool registers"),
		Orphan->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_cell")), 120, 60));
	FItemInstance OrphanInstance = MakeInstance(0x26, ItemRangedTest);
	TestTrue(TEXT("the orphan binds the ranged weapon"), Orphan->ApplyEquippedWeapon(&OrphanInstance).bWeaponBound);
	double OrphanNow = 100.0;
	Orphan->SetFireClockProvider([&OrphanNow]() { return OrphanNow; });

	FFireIntent OrphanIntent;
	OrphanIntent.WeaponInstanceId = OrphanInstance.InstanceId;
	OrphanIntent.LocalShotSequence = 1;
	const FFireOutcome Unallocated = Orphan->TryFire(OrphanIntent);
	TestEqual(TEXT("the unregistered source names the allocation"), static_cast<int32>(Unallocated.Reject),
		static_cast<int32>(EFireReject::SequenceAllocationFailed));
	const FWeaponBindingRecord* OrphanRecord = Orphan->GetActiveBinding();
	TestNotNull(TEXT("the orphan binding survived the rollback"), OrphanRecord);
	if (OrphanRecord != nullptr)
	{
		TestEqual(TEXT("the deducted round rolled back"), OrphanRecord->LoadedRounds, 12);
	}
	TestEqual(TEXT("the failed allocation committed nothing"), Orphan->GetCommittedShotCount(), 0);

	// Registration mints the source and the same intent fires for real.
	FCombatEntityMetadata Meta;
	Meta.Faction = TEXT("player");
	Meta.Category = TEXT("player");
	TestTrue(TEXT("the source registration mints an id"), IsValidCombatEntityId(Orphan->RegisterFireSource(Meta)));
	const FFireOutcome OrphanShot = Orphan->TryFire(OrphanIntent);
	TestTrue(TEXT("the registered orphan fires"), OrphanShot.bFired);
	TestEqual(TEXT("the orphan shot takes sequence 1"), OrphanShot.Shot.ShotId, static_cast<FShotId>(1));
	const FWeaponBindingRecord* OrphanAfter = Orphan->GetActiveBinding();
	if (OrphanAfter != nullptr)
	{
		TestEqual(TEXT("the registered shot deducted once"), OrphanAfter->LoadedRounds, 11);
	}
	return true;
}

/**
 * Acceptance: the component-level reload wrappers refill the fire book - the
 * M5-021 reload ledger and the M5-019 fire book meet here, the open window
 * gates fire and repeated refill cycles work.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_022ReloadWrappersRefillFireBook,
	"UEMMO.Tasks.M5_022.ReloadWrappersRefillFireBook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_022ReloadWrappersRefillFireBook::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// Drain three rounds (one per pacing window).
	for (int32 Index = 1; Index <= 3; ++Index)
	{
		const FFireOutcome Shot = Rig.Fire(static_cast<uint64>(Index));
		if (!TestTrue(FString::Printf(TEXT("drain shot %d commits"), Index), Shot.bFired))
		{
			return false;
		}
		Rig.Mount->ReleaseFire();
		Rig.Now += 1.0;
	}
	TestEqual(TEXT("three rounds left the book"), Rig.Rounds(), 9);

	// The reload cycle: open, fire gated, complete, book refilled.
	const FAmmoReloadOutcome Window = Rig.Mount->BeginReload(Rig.Now, 5.0);
	if (!TestTrue(TEXT("the reload window opens"), Window.bSuccess))
	{
		AddError(FString::Printf(TEXT("BeginReload refused: %s"), *Window.ErrorDetail));
		return false;
	}
	TestEqual(TEXT("the window reports the fire book"), Window.LoadedRounds, 9);
	const FFireOutcome GatedFire = Rig.Fire(4);
	TestEqual(TEXT("the open window gates the fire"), static_cast<int32>(GatedFire.Reject),
		static_cast<int32>(EFireReject::Reloading));
	const FAmmoReloadOutcome Filled = Rig.Mount->CompleteReload();
	if (!TestTrue(TEXT("the completion grants the rounds"), Filled.bSuccess))
	{
		AddError(FString::Printf(TEXT("CompleteReload refused: %s"), *Filled.ErrorDetail));
		return false;
	}
	TestEqual(TEXT("the grant is min(capacity - loaded, reserve)"), Filled.RoundsTransferred, 3);
	TestEqual(TEXT("the fire book synced to the refill"), Rig.Rounds(), 12);
	TestEqual(TEXT("the shared pool drained by the grant"),
		Rig.Mount->GetAmmoModel().GetReserveRounds(FName(TEXT("ammo_cell"))), 57);

	// The refilled book fires again.
	const FFireOutcome AfterRefill = Rig.Fire(4);
	TestTrue(TEXT("the refilled book fires"), AfterRefill.bFired);
	Rig.Mount->ReleaseFire();
	Rig.Now += 1.0;

	// The SECOND full cycle (the fresh per-cycle slot identity): drain to
	// empty, reload, fire - the loop does not lock after the first refill.
	for (int32 Index = 5; Index <= 15; ++Index)
	{
		const FFireOutcome Shot = Rig.Fire(static_cast<uint64>(Index));
		if (!TestTrue(FString::Printf(TEXT("drain shot %d commits"), Index), Shot.bFired))
		{
			return false;
		}
		Rig.Mount->ReleaseFire();
		Rig.Now += 1.0;
	}
	TestEqual(TEXT("the book drained to empty again"), Rig.Rounds(), 0);
	const FAmmoReloadOutcome SecondWindow = Rig.Mount->BeginReload(Rig.Now, 5.0);
	if (!TestTrue(TEXT("the second cycle window opens"), SecondWindow.bSuccess))
	{
		AddError(FString::Printf(TEXT("second BeginReload refused: %s"), *SecondWindow.ErrorDetail));
		return false;
	}
	const FAmmoReloadOutcome SecondFill = Rig.Mount->CompleteReload();
	TestTrue(TEXT("the second cycle completes"), SecondFill.bSuccess);
	TestEqual(TEXT("the second grant fills the magazine"), SecondFill.RoundsTransferred, 12);
	TestEqual(TEXT("the fire book is full again"), Rig.Rounds(), 12);
	TestEqual(TEXT("the pool drained both cycles"),
		Rig.Mount->GetAmmoModel().GetReserveRounds(FName(TEXT("ammo_cell"))), 45);
	const FFireOutcome AfterSecondCycle = Rig.Fire(16);
	TestTrue(TEXT("the second cycle fires too"), AfterSecondCycle.bFired);
	return true;
}

/**
 * Acceptance: death voids the in-flight shot, closes the books and refuses
 * fire while dead; after the revive the same binding fires legally again
 * ("空弹和死亡无发射，恢复后可合法开火").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_022DeathVoidAndRecovery,
	"UEMMO.Tasks.M5_022.DeathVoidAndRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_022DeathVoidAndRecovery::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}
	const uint64 GenerationBefore = Rig.Mount->GetBindingGeneration();

	const FFireOutcome FirstShot = Rig.Fire(1);
	TestTrue(TEXT("the first shot commits"), FirstShot.bFired);
	TestTrue(TEXT("the shot is in flight"), Rig.Mount->IsShotInFlight());

	// Death mid-flight: the shot voids, the cooldown resets, fire disables and
	// the generation bumps (captured callbacks drop their work).
	Rig.Mount->NotifyOwnerDied();
	TestFalse(TEXT("death voided the in-flight shot"), Rig.Mount->IsShotInFlight());
	TestEqual(TEXT("death reset the cooldown"), Rig.Mount->GetNextFireTimeSeconds(), 0.0);
	TestFalse(TEXT("death disables fire"), Rig.Mount->IsFireAuthorized());
	TestFalse(TEXT("death bumped the generation"), Rig.Mount->IsBindingGenerationCurrent(GenerationBefore));

	// Dead pulls refuse; the books stay at the death state.
	const FFireOutcome DeadFire = Rig.Fire(2);
	TestEqual(TEXT("the dead pull refuses"), static_cast<int32>(DeadFire.Reject),
		static_cast<int32>(EFireReject::OwnerDead));
	TestEqual(TEXT("the dead pull committed nothing"), Rig.Mount->GetCommittedShotCount(), 1);
	TestEqual(TEXT("the dead pull deducted nothing"), Rig.Rounds(), 11);
	const FAmmoReloadOutcome DeadReload = Rig.Mount->BeginReload(Rig.Now, 5.0);
	TestEqual(TEXT("the dead reload refuses"), static_cast<int32>(DeadReload.Error),
		static_cast<int32>(EAmmoModelError::UnknownInstance));

	// The revive re-entry: the surviving binding fires the next sequence.
	Rig.Mount->NotifyOwnerRevived();
	Rig.Now += 1.0;
	const FFireOutcome RevivedFire = Rig.Fire(2);
	TestTrue(TEXT("the revived mount fires legally"), RevivedFire.bFired);
	TestEqual(TEXT("the revived shot takes sequence 2"), RevivedFire.Shot.ShotId, static_cast<FShotId>(2));
	TestEqual(TEXT("the revived mount counted both shots"), Rig.Mount->GetCommittedShotCount(), 2);
	TestEqual(TEXT("the revived shot deducted once"), Rig.Rounds(), 10);
	return true;
}

#endif
