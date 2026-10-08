// M5-024: the burst-fire policy (M5 interface contract section 3, the 022
// IFirePolicy mount point). Pins Weapons/FirePolicies/BurstFirePolicy through
// the real component transaction:
//
// - One press arms one bounded burst ("一次意图一串"): every sub-shot is
//   individually admitted, deducted and numbered through the single-fire
//   transaction (own ShotId, own round).
// - A repeated press edge or a hold never stacks a second burst; after the
//   burst completes only a fresh press re-arms.
// - A started burst runs to its end after the release ("已开始点射松键可继
//   续"); a release before the first sub-shot disarms the arm.
// - Insufficient rounds fire only the legal sub-shots, the rest is the named
//   MagazineEmpty refusal - no negative ammo.
// - Death/equipment-switch/reload cancel the pending remainder through the
//   caller's CancelPendingBurst duty (the component's own gates refuse the
//   stream meanwhile) - the stale sub-shots never resume ("中断不补发").
// - Pause freezes the burst in place; a stalled frame fires at most the
//   single due sub-shot; every shot goes through the 022 transaction.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Weapons/WeaponComponent.h"
#include "../Weapons/FirePolicies/BurstFirePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_024
{
	/** The sub-shot interval of the fixture (400 RPM = 0.15 s). */
	constexpr double SubShotInterval = 0.15;

	/** Comparison tolerance for the double clock arithmetic. */
	constexpr double ClockEpsilon = 1e-9;

	/**
	 * The combat catalog fixture (the 019/023 style): the legacy four-attack
	 * training sword, the 30-round hitscan burst row on ammo_cell at 400 RPM,
	 * and a projectile-mode row (the mode-mismatch probe).
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

		FWeaponDefinition Burst;
		Burst.WeaponId = TEXT("weapon_burst_sample");
		Burst.FireMode = EWeaponFireMode::Hitscan;
		Burst.DamageProfileId = TEXT("light_02");
		Burst.AmmoId = TEXT("ammo_cell");
		Burst.MagazineSize = 30;
		Burst.FireRateRpm = 400.0f;
		Burst.SpreadDegrees = 1.0f;
		Burst.RangeCm = 5000.0f;
		Parsed.Weapons.Add(Burst);

		FWeaponDefinition ProjectileWeapon;
		ProjectileWeapon.WeaponId = TEXT("weapon_projectile_sample");
		ProjectileWeapon.FireMode = EWeaponFireMode::Projectile;
		ProjectileWeapon.AmmoId = TEXT("ammo_cell");
		ProjectileWeapon.MagazineSize = 6;
		ProjectileWeapon.FireRateRpm = 120.0f;
		ProjectileWeapon.ProjectileId = TEXT("projectile_sample");
		Parsed.Weapons.Add(ProjectileWeapon);

		FProjectileDefinition Bullet;
		Bullet.ProjectileId = TEXT("projectile_sample");
		Bullet.Motion = EProjectileMotion::Straight;
		Bullet.SpeedCmS = 3000.0f;
		Bullet.LifetimeS = 3.0f;
		Bullet.DamageProfileId = TEXT("light_01");
		Parsed.Projectiles.Add(Bullet);

		FAmmoType Cell;
		Cell.AmmoId = TEXT("ammo_cell");
		Cell.MaxReserve = 120;
		Cell.MagazineSize = 30;
		Parsed.AmmoTypes.Add(Cell);

		return FCombatCatalog::BuildFromParsed(Parsed, OutCatalog, OutError);
	}

	/** Item id constants shared by the tests. */
	const FName ItemTrainingSword = TEXT("weapon_training");
	const FName ItemBurstTest = TEXT("weapon_burst_test");
	const FName ItemProjectileTest = TEXT("weapon_projectile_test");

	/** The item catalog fixture: mapped sword/burst/projectile items. */
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

		FItemDefinition Burst;
		Burst.DefinitionId = ItemBurstTest;
		Burst.DisplayName = TEXT("Burst Test Gun");
		Burst.Slot = EItemSlot::Weapon;
		Burst.BaseStats.Attack = 2.0f;
		Burst.Rarity = EItemRarity::Normal;
		Burst.WeaponDefinitionId = TEXT("weapon_burst_sample");
		Catalog.AddDefinition(Burst);

		FItemDefinition Projectile;
		Projectile.DefinitionId = ItemProjectileTest;
		Projectile.DisplayName = TEXT("Projectile Test Gun");
		Projectile.Slot = EItemSlot::Weapon;
		Projectile.BaseStats.Attack = 2.0f;
		Projectile.Rarity = EItemRarity::Normal;
		Projectile.WeaponDefinitionId = TEXT("weapon_projectile_sample");
		Catalog.AddDefinition(Projectile);

		return Catalog;
	}

	/** Deterministic item instance (the M3 fixture style, M5-024 seeds). */
	FItemInstance MakeInstance(uint32 Seed, const FName DefinitionId)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x05240000u + Seed, 0x0BECu, Seed + 20u, Seed * 7u + 5u);
		Instance.DefinitionId = DefinitionId;
		Instance.RollSeed = static_cast<int64>(0x524 + Seed);
		Instance.Level = 1;
		return Instance;
	}

	/**
	 * The ready-to-fire rig: a standalone mount, the fixture pair, a ranged
	 * bind (the fire book starts full at 30), a registered fire source, a
	 * manually advanced fire clock and the burst policy registered.
	 */
	struct FFireRig
	{
		UWeaponComponent* Mount = nullptr;
		FCombatCatalog Catalog;
		FItemDefinitionCatalog Items;
		FItemInstance RangedInstance;
		FBurstFirePolicy BurstPolicy;
		double Now = 100.0;
		uint64 NextLocalSequence = 0;
		TArray<double> ShotTimes;

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
			RangedInstance = MakeInstance(1, ItemBurstTest);
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
			Mount->SetFirePolicy(&BurstPolicy);
			Mount->OnShotCommitted.AddLambda([this](const FShotContext& Shot)
			{
				ShotTimes.Add(Shot.CommittedAtSeconds);
			});
			BurstPolicy.SetBurstCount(3);
			BurstPolicy.SetSubShotIntervalSeconds(SubShotInterval);
			return true;
		}

		/** A fresh trigger press edge (arms one burst while idle). */
		void Press()
		{
			BurstPolicy.NotifyTriggerPressed();
		}

		/** A trigger release edge (disarms a burst that has not started). */
		void ReleaseTrigger()
		{
			BurstPolicy.NotifyTriggerReleased();
		}

		FFireOutcome Fire()
		{
			FFireIntent Intent;
			Intent.WeaponInstanceId = RangedInstance.InstanceId;
			Intent.LocalShotSequence = ++NextLocalSequence;
			return Mount->TryFire(Intent);
		}

		/**
		 * One input poll at the current frame: TryFire, and the in-flight
		 * window is closed the same frame (hitscan resolves instantly).
		 */
		bool Poll()
		{
			const FFireOutcome Outcome = Fire();
			if (Outcome.bFired)
			{
				Mount->ReleaseFire();
				return true;
			}
			return false;
		}

		/** Advances the frame clock by DeltaSeconds and polls once. */
		bool Frame(const double DeltaSeconds)
		{
			Now += DeltaSeconds;
			return Poll();
		}

		/** Polls FrameCount frames; returns the committed shots. */
		int32 Hold(const int32 FrameCount, const double DeltaSeconds)
		{
			int32 Fired = 0;
			for (int32 Index = 0; Index < FrameCount; ++Index)
			{
				if (Frame(DeltaSeconds))
				{
					++Fired;
				}
			}
			return Fired;
		}

		/** Advances the frame clock without polling. */
		void Idle(const int32 FrameCount, const double DeltaSeconds)
		{
			for (int32 Index = 0; Index < FrameCount; ++Index)
			{
				Now += DeltaSeconds;
			}
		}

		/** The fire book (the M5-019 slot) of the active binding. */
		int32 Rounds() const
		{
			const FWeaponBindingRecord* Binding = Mount->GetActiveBinding();
			return Binding != nullptr ? Binding->LoadedRounds : -1;
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_024;

/**
 * Acceptance: one press fires exactly one burst of three - each sub-shot is
 * its own transaction (own ShotId, own round), the follow-ups arrive one
 * interval apart, and neither a hold nor a completed burst stacks another
 * string; a fresh press starts a fresh burst.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_024PressFiresOneBurstOfThree,
	"UEMMO.Tasks.M5_024.PressFiresOneBurstOfThree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_024PressFiresOneBurstOfThree::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// Polls without a press never fire ("一次意图一串" needs the edge).
	TestEqual(TEXT("an unpressed hold fires nothing"), Rig.Hold(10, 1.0 / 60.0), 0);
	TestEqual(TEXT("the fire book is untouched"), Rig.Rounds(), 30);

	// The press arms exactly one burst: the first sub-shot fires on the next
	// poll, the follow-ups one interval apart, then the string ends.
	Rig.Press();
	TestEqual(TEXT("the first sub-shot fires"), Rig.Hold(1, 1.0 / 60.0), 1);
	TestEqual(TEXT("the fire book moved one round"), Rig.Rounds(), 29);
	TestEqual(TEXT("the frames before the next sub-shot stay quiet"), Rig.Hold(8, 1.0 / 60.0), 0);
	TestEqual(TEXT("the second sub-shot fires"), Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the frames before the last sub-shot stay quiet"), Rig.Hold(8, 1.0 / 60.0), 0);
	TestEqual(TEXT("the third sub-shot fires"), Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the burst is exactly three shots"), Rig.Mount->GetCommittedShotCount(), 3);
	TestEqual(TEXT("each sub-shot deducted its own round"), Rig.Rounds(), 27);

	// Holding past the completed burst stacks nothing.
	TestEqual(TEXT("the held frames after the burst fire nothing"), Rig.Hold(20, 1.0 / 60.0), 0);
	TestEqual(TEXT("the count stays at three"), Rig.Mount->GetCommittedShotCount(), 3);

	// Every sub-shot went through the atomic transaction (one common ShotId
	// each, the delegate saw exactly the commits).
	TestEqual(TEXT("the delegate saw every committed sub-shot"),
		Rig.ShotTimes.Num(), Rig.Mount->GetCommittedShotCount());

	// A fresh press starts a fresh burst of three.
	Rig.Press();
	TestEqual(TEXT("the fresh burst opens"), Rig.Hold(1, 1.0 / 60.0), 1);
	TestEqual(TEXT("the fresh burst continues"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the fresh burst finishes"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("two presses fired exactly six sub-shots"), Rig.Mount->GetCommittedShotCount(), 6);
	TestEqual(TEXT("the fire book matches the sub-shots"), Rig.Rounds(), 24);
	return true;
}

/**
 * Acceptance: repeated press edges and holds never stack a second burst - a
 * press edge while a burst is pending is ignored, a double arm is one burst,
 * and the completed burst needs a fresh press edge to re-arm.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_024RepeatPressAndHoldDoNotStack,
	"UEMMO.Tasks.M5_024.RepeatPressAndHoldDoNotStack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_024RepeatPressAndHoldDoNotStack::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// A double press edge before the first poll still arms one burst only.
	Rig.Press();
	Rig.Press();
	TestEqual(TEXT("the double-armed press fires one first sub-shot"), Rig.Hold(1, 1.0 / 60.0), 1);

	// A press edge mid-burst is ignored: the string stays three.
	Rig.Press();
	TestEqual(TEXT("the second sub-shot fires once"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	Rig.Press();
	TestEqual(TEXT("the third sub-shot fires once"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the burst is still exactly three shots"), Rig.Mount->GetCommittedShotCount(), 3);

	// Holding past the end never starts a second string.
	TestEqual(TEXT("the held frames fire nothing"), Rig.Hold(20, 1.0 / 60.0), 0);
	TestEqual(TEXT("the count is unchanged"), Rig.Mount->GetCommittedShotCount(), 3);
	TestEqual(TEXT("the fire book moved three rounds"), Rig.Rounds(), 27);
	return true;
}

/**
 * Acceptance: a release before the first sub-shot disarms the arm entirely;
 * a release after the burst started lets it run to its end ("已开始点射松键
 * 可继续").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_024ReleaseBeforeStartCancelsAfterStartContinues,
	"UEMMO.Tasks.M5_024.ReleaseBeforeStartCancelsAfterStartContinues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_024ReleaseBeforeStartCancelsAfterStartContinues::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// Press + release before any poll: the arm is gone, nothing fires.
	Rig.Press();
	Rig.ReleaseTrigger();
	TestEqual(TEXT("the disarmed hold fires nothing"), Rig.Hold(10, 1.0 / 60.0), 0);
	TestEqual(TEXT("the fire book is untouched"), Rig.Rounds(), 30);

	// A started burst survives the release and runs to its end.
	Rig.Press();
	TestEqual(TEXT("the burst starts"), Rig.Hold(1, 1.0 / 60.0), 1);
	Rig.ReleaseTrigger();
	TestEqual(TEXT("the second sub-shot still fires"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the third sub-shot still fires"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the released burst ran to three"), Rig.Mount->GetCommittedShotCount(), 3);
	TestEqual(TEXT("the fire book moved three rounds"), Rig.Rounds(), 27);
	return true;
}

/**
 * Acceptance: with only two rounds in the fire book the burst fires exactly
 * the two legal sub-shots and refuses the third with the named MagazineEmpty -
 * no negative ammo, no partial state ("备弹不足只发合法子发"); the reload
 * cancel duty keeps the stale remainder from resuming after the refill.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_024InsufficientRoundsFireLegalSubShotsOnly,
	"UEMMO.Tasks.M5_024.InsufficientRoundsFireLegalSubShotsOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_024InsufficientRoundsFireLegalSubShotsOnly::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// The supply lineage simulation: the guarded registry write pins the fire
	// book at two rounds.
	FString BookError;
	TestTrue(TEXT("the fire book is pinned at two rounds"),
		Rig.Mount->GetRegistry().SetInstanceRounds(Rig.RangedInstance.InstanceId, 2, &BookError));

	// The burst fires the two legal sub-shots; the third is the named refusal.
	Rig.Press();
	TestEqual(TEXT("the first legal sub-shot fires"), Rig.Hold(1, 1.0 / 60.0), 1);
	TestEqual(TEXT("the second legal sub-shot fires"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the fire book is empty"), Rig.Rounds(), 0);
	const int32 CountWhenDry = Rig.Mount->GetCommittedShotCount();

	// The third sub-shot is due but the book is dry: push the fire clock past
	// the component cooldown laid by the second commit, then the named
	// MagazineEmpty refusal, zero side effects, no negative ammo.
	Rig.Now += 0.2;
	const FFireOutcome Dry = Rig.Fire();
	TestFalse(TEXT("the third sub-shot fires nothing"), Dry.bFired);
	TestEqual(TEXT("the dry sub-shot names the empty magazine"), static_cast<int32>(Dry.Reject),
		static_cast<int32>(EFireReject::MagazineEmpty));
	TestEqual(TEXT("the dry refusal moved no shots"), Rig.Mount->GetCommittedShotCount(), CountWhenDry);
	TestEqual(TEXT("the fire book holds no negative rounds"), Rig.Rounds(), 0);
	TestEqual(TEXT("no shot bypassed the atomic commit"),
		Rig.ShotTimes.Num(), Rig.Mount->GetCommittedShotCount());

	// The pending remainder is cancelled on the reload (the caller duty) and
	// never resumes after the refill; a fresh press fires a fresh burst.
	Rig.BurstPolicy.CancelPendingBurst();
	const FAmmoReloadOutcome Open = Rig.Mount->BeginReload(Rig.Now, 0.5);
	TestTrue(TEXT("the reload window opens"), Open.bSuccess);
	TestTrue(TEXT("the reload completes"), Rig.Mount->CompleteReload().bSuccess);
	TestEqual(TEXT("the refilled fire book is full"), Rig.Rounds(), 30);
	TestEqual(TEXT("the stale sub-shot never resumed"), Rig.Hold(10, 1.0 / 60.0), 0);
	Rig.Press();
	TestEqual(TEXT("the fresh burst starts"), Rig.Hold(1, 1.0 / 60.0), 1);
	return true;
}

/**
 * Acceptance: death and equipment switch cancel the pending remainder - the
 * component's own gates refuse the stream meanwhile, the cancelled sub-shots
 * never resume, and a fresh press after the recovery starts a fresh burst
 * ("死亡/切换/换弹取消剩余，中断不补发").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_024DeathAndSwitchCancelPendingSubShots,
	"UEMMO.Tasks.M5_024.DeathAndSwitchCancelPendingSubShots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_024DeathAndSwitchCancelPendingSubShots::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}

	// A burst two sub-shots deep, one still pending.
	Rig.Press();
	TestEqual(TEXT("the burst starts"), Rig.Hold(1, 1.0 / 60.0), 1);
	TestEqual(TEXT("the second sub-shot fires"), Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
	const int32 CountBeforeDeath = Rig.Mount->GetCommittedShotCount();

	// Death: the component gate refuses the stream; the caller cancels the
	// stale remainder.
	Rig.Mount->NotifyOwnerDied();
	const FFireOutcome Dead = Rig.Fire();
	TestEqual(TEXT("the dead wielder refuses the fire"), static_cast<int32>(Dead.Reject),
		static_cast<int32>(EFireReject::OwnerDead));
	Rig.BurstPolicy.CancelPendingBurst();
	Rig.Mount->NotifyOwnerRevived();
	TestEqual(TEXT("the stale sub-shot never resumes after the revive"), Rig.Hold(10, 1.0 / 60.0), 0);
	TestEqual(TEXT("the count is unchanged by the interruption"), Rig.Mount->GetCommittedShotCount(), CountBeforeDeath);

	// A fresh press after the recovery starts a fresh burst.
	Rig.Press();
	TestEqual(TEXT("the fresh burst starts after the revive"), Rig.Hold(1, 1.0 / 60.0), 1);

	// Equipment switch mid-burst: the teardown tears the binding, the caller
	// cancels the remainder, the rebind restores the book and no stale
	// sub-shot resumes.
	TestTrue(TEXT("the switch target binds"),
		Rig.Mount->ApplyEquippedWeapon(nullptr).bSucceeded);
	Rig.BurstPolicy.CancelPendingBurst();
	FItemInstance Rebound = MakeInstance(2, ItemBurstTest);
	TestTrue(TEXT("the burst weapon rebinds"), Rig.Mount->ApplyEquippedWeapon(&Rebound).bWeaponBound);
	Rig.RangedInstance = Rebound;
	TestEqual(TEXT("no stale sub-shot resumes after the switch"), Rig.Hold(10, 1.0 / 60.0), 0);
	Rig.Press();
	TestEqual(TEXT("the fresh burst starts after the switch"), Rig.Hold(1, 1.0 / 60.0), 1);
	TestEqual(TEXT("every shot went through the atomic commit"),
		Rig.ShotTimes.Num(), Rig.Mount->GetCommittedShotCount());
	return true;
}

/**
 * Acceptance: a pause freezes the burst in place (no shots, no catch-up) and
 * a stalled frame fires at most the single due sub-shot; the burst completes
 * normally after both and every sub-shot stays inside the atomic transaction
 * ("暂停/卡顿边界一致").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_024PauseAndStallBoundaries,
	"UEMMO.Tasks.M5_024.PauseAndStallBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_024PauseAndStallBoundaries::RunTest(const FString& Parameters)
{
	// Pause: the frozen clock holds the pending sub-shots in place.
	{
		FFireRig Rig;
		FString RigError;
		if (!TestTrue(TEXT("the pause rig builds"), Rig.Build(RigError)))
		{
			AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
			return false;
		}
		Rig.Press();
		TestEqual(TEXT("the burst starts"), Rig.Hold(1, 1.0 / 60.0), 1);
		for (int32 Index = 0; Index < 10; ++Index)
		{
			TestFalse(TEXT("a paused poll never fires"), Rig.Poll());
		}
		TestEqual(TEXT("the pause produced no shots"), Rig.Mount->GetCommittedShotCount(), 1);
		TestEqual(TEXT("the follow-up fires at the boundary after the lift"),
			Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
		TestEqual(TEXT("the last sub-shot completes the burst"),
			Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
		TestEqual(TEXT("the paused burst ran to three"), Rig.Mount->GetCommittedShotCount(), 3);
		TestEqual(TEXT("the fire book moved three rounds"), Rig.Rounds(), 27);
	}

	// Stall: one long frame fires at most the single due sub-shot - the
	// skipped shots are dropped, never accumulated.
	{
		FFireRig Rig;
		FString RigError;
		if (!TestTrue(TEXT("the stall rig builds"), Rig.Build(RigError)))
		{
			AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
			return false;
		}
		Rig.Press();
		TestEqual(TEXT("the burst starts"), Rig.Hold(1, 1.0 / 60.0), 1);
		TestTrue(TEXT("the stalled frame admits the single due sub-shot"), Rig.Frame(0.5));
		TestEqual(TEXT("the stall fired exactly one sub-shot"), Rig.Mount->GetCommittedShotCount(), 2);
		TestEqual(TEXT("the same-frame double poll fires nothing"), Rig.Poll(), false);
		TestEqual(TEXT("the last sub-shot completes at its own interval"),
			Rig.Hold(8, 1.0 / 60.0) + Rig.Hold(2, 1.0 / 60.0), 1);
		TestEqual(TEXT("the stalled burst ran to three"), Rig.Mount->GetCommittedShotCount(), 3);
		TestEqual(TEXT("the post-burst stall fires nothing"), Rig.Frame(0.5), false);
		TestEqual(TEXT("every shot went through the atomic commit"),
			Rig.ShotTimes.Num(), Rig.Mount->GetCommittedShotCount());
		TestEqual(TEXT("the fire book matches the sub-shots"), Rig.Rounds(), 27);
	}
	return true;
}

#endif
