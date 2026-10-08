// M5-023: the automatic-fire pacing policy (M5 interface contract section 3,
// the 022 IFirePolicy mount point). Pins Weapons/FirePolicies/AutomaticFirePolicy
// through the real component transaction:
//
// - Hold-to-fire: while the trigger stays down the input layer polls TryFire
//   once per frame (closing the per-shot in-flight window the same frame) and
//   the policy admits one shot per configured interval on the component's
//   injected fire clock (GameClockSeconds semantics, Pause-frozen). Counts at
//   30/60/120 FPS stay inside the interval boundaries ("0.15秒配置在30/60/120
//   FPS射击数符合区间边界").
// - No catch-up: a stall fires exactly ONE shot when it ends - the skipped
//   shots are dropped, never accumulated, and the rounds move one per shot
//   ("100ms/长卡顿不集中扣多弹").
// - Pause freezes the cadence (the fire clock stops advancing); a per-character
//   HitStop does not exist on the fire path - the gun keeps the World-clock
//   cadence ("暂停不推进冷却，单角色HitStop不改变枪冷却").
// - Release stops the stream and leaves no residue; reload/switch/death
//   terminate through the component's own named gates.
// - Empty magazine and every refusal stay named and side-effect free; the
//   automatic stream never bypasses the atomic commit (one deduction + one
//   common ActionSequence per shot).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Weapons/WeaponComponent.h"
#include "../Weapons/FirePolicies/AutomaticFirePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_023
{
	/** The configured shot interval of the fixture weapon (400 RPM = 0.15 s). */
	constexpr double ShotInterval = 0.15;

	/** Comparison tolerance for the double clock arithmetic. */
	constexpr double ClockEpsilon = 1e-9;

	/**
	 * The combat catalog fixture (the 019/022 style): the legacy four-attack
	 * training sword, the 30-round AUTOMATIC hitscan row on ammo_cell at
	 * 400 RPM (0.15 s), and a projectile-mode row (the mode-mismatch probe)
	 * whose damage authority is the projectile definition.
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

		FWeaponDefinition Auto;
		Auto.WeaponId = TEXT("weapon_auto_sample");
		Auto.FireMode = EWeaponFireMode::Hitscan;
		Auto.DamageProfileId = TEXT("light_02");
		Auto.AmmoId = TEXT("ammo_cell");
		Auto.MagazineSize = 30;
		Auto.FireRateRpm = 400.0f;
		Auto.SpreadDegrees = 1.0f;
		Auto.RangeCm = 5000.0f;
		Parsed.Weapons.Add(Auto);

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
	const FName ItemAutoTest = TEXT("weapon_auto_test");
	const FName ItemProjectileTest = TEXT("weapon_projectile_test");

	/** The item catalog fixture: mapped sword/auto/projectile items. */
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

		FItemDefinition Auto;
		Auto.DefinitionId = ItemAutoTest;
		Auto.DisplayName = TEXT("Auto Test Gun");
		Auto.Slot = EItemSlot::Weapon;
		Auto.BaseStats.Attack = 2.0f;
		Auto.Rarity = EItemRarity::Normal;
		Auto.WeaponDefinitionId = TEXT("weapon_auto_sample");
		Catalog.AddDefinition(Auto);

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

	/** Deterministic item instance (the M3 fixture style, M5-023 seeds). */
	FItemInstance MakeInstance(uint32 Seed, const FName DefinitionId)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x05230000u + Seed, 0x0BECu, Seed + 20u, Seed * 7u + 5u);
		Instance.DefinitionId = DefinitionId;
		Instance.RollSeed = static_cast<int64>(0x523 + Seed);
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
	 * bind (the fire book starts full at 30), a registered fire source, a
	 * manually advanced fire clock and the automatic policy registered.
	 */
	struct FFireRig
	{
		UWeaponComponent* Mount = nullptr;
		FCombatCatalog Catalog;
		FItemDefinitionCatalog Items;
		FItemInstance RangedInstance;
		FAutomaticFirePolicy AutoPolicy;
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
			RangedInstance = MakeInstance(1, ItemAutoTest);
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
			Mount->SetFirePolicy(&AutoPolicy);
			Mount->OnShotCommitted.AddLambda([this](const FShotContext& Shot)
			{
				ShotTimes.Add(Shot.CommittedAtSeconds);
			});
			return true;
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
		 * window is closed the same frame (hitscan resolves instantly). The
		 * M5-033 production loop follows exactly this pattern while the
		 * trigger stays down.
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

		/** Advances the frame clock without polling (the trigger is up). */
		void Idle(const int32 FrameCount, const double DeltaSeconds)
		{
			for (int32 Index = 0; Index < FrameCount; ++Index)
			{
				Now += DeltaSeconds;
			}
		}

		/** Holds the trigger for FrameCount frames; returns the committed shots. */
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

		/** The fire book (the M5-019 slot) of the active binding. */
		int32 Rounds() const
		{
			const FWeaponBindingRecord* Binding = Mount->GetActiveBinding();
			return Binding != nullptr ? Binding->LoadedRounds : -1;
		}

		/** True when every pair of consecutive shot times is at least Interval apart. */
		bool SpacingHolds(const double Interval, double& OutWorstGap) const
		{
			OutWorstGap = TNumericLimits<double>::Max();
			for (int32 Index = 1; Index < ShotTimes.Num(); ++Index)
			{
				const double Gap = ShotTimes[Index] - ShotTimes[Index - 1];
				OutWorstGap = FMath::Min(OutWorstGap, Gap);
				if (Gap < Interval - ClockEpsilon)
				{
					return false;
				}
			}
			return ShotTimes.Num() > 0;
		}

		/**
		 * The legal shot-count band of a hold: the cadence can quantize up to
		 * one frame per shot (Ceil(Interval/Delta) frames between shots) and
		 * can never run faster than the configured interval.
		 */
		void CountBand(const int32 FrameCount, const double DeltaSeconds, const double Interval, int32& OutMin, int32& OutMax) const
		{
			const int32 QuantizedStep = FMath::Max(1, FMath::CeilToInt((Interval / DeltaSeconds) + ClockEpsilon));
			OutMin = 1 + ((FrameCount - 1) / QuantizedStep);
			OutMax = 1 + static_cast<int32>(FMath::Floor(((FrameCount - 1) * DeltaSeconds + ClockEpsilon) / (Interval - ClockEpsilon)));
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_023;

/**
 * Acceptance: holding the trigger fires at the configured interval on every
 * frame rate - counts inside the interval boundaries at 30/60/120 FPS, shot
 * spacing never below the interval, one deduction per shot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_023HoldIntervalAcrossFrameRates,
	"UEMMO.Tasks.M5_023.HoldIntervalAcrossFrameRates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_023HoldIntervalAcrossFrameRates::RunTest(const FString& Parameters)
{
	struct FFrameRateCase
	{
		const TCHAR* Label;
		int32 FrameCount;
		double DeltaSeconds;
		int32 ExpectedMin;
		int32 ExpectedMax;
	};

	// 2 seconds of holding per rate. 30 FPS quantizes the 0.15 s interval to
	// a deterministic 5-frame step (12 shots); 60/120 FPS sit exactly on the
	// interval boundary, so the honest assertion is the boundary band.
	const TArray<FFrameRateCase> Cases = {
		{ TEXT("30 FPS"), 60, 1.0 / 30.0, 12, 14 },
		{ TEXT("60 FPS"), 120, 1.0 / 60.0, 12, 14 },
		{ TEXT("120 FPS"), 240, 1.0 / 120.0, 13, 14 },
	};

	for (const FFrameRateCase& Case : Cases)
	{
		FFireRig Rig;
		FString RigError;
		if (!TestTrue(FString::Printf(TEXT("the %s rig builds"), Case.Label), Rig.Build(RigError)))
		{
			AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
			return false;
		}
		Rig.AutoPolicy.SetShotIntervalSeconds(ShotInterval);

		const int32 Fired = Rig.Hold(Case.FrameCount, Case.DeltaSeconds);
		TestTrue(FString::Printf(TEXT("the %s count is inside [%d..%d]"), Case.Label, Case.ExpectedMin, Case.ExpectedMax),
			Fired >= Case.ExpectedMin && Fired <= Case.ExpectedMax);

		double WorstGap;
		TestTrue(FString::Printf(TEXT("the %s spacing never undercuts the interval"), Case.Label),
			Rig.SpacingHolds(ShotInterval, WorstGap));

		// One shot is one deduction and one common sequence: the book moved
		// exactly with the shots and the ShotIds stayed consecutive (the rig
		// started full at 30 rounds).
		TestEqual(FString::Printf(TEXT("the %s fire book moved one round per shot"), Case.Label),
			Rig.Rounds(), 30 - Fired);
		TestEqual(FString::Printf(TEXT("the %s commit count matches"), Case.Label),
			Rig.Mount->GetCommittedShotCount(), Fired);
	}
	return true;
}

/**
 * Acceptance: a stall (a long frame gap) fires at most ONE shot when it ends -
 * the skipped shots are dropped, never accumulated, the rounds move one per
 * shot, and the second poll of the same frame is the named cooldown refusal
 * with zero side effects ("卡顿不补射历史整串").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_023StallFiresAtMostOneShot,
	"UEMMO.Tasks.M5_023.StallFiresAtMostOneShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_023StallFiresAtMostOneShot::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}
	Rig.AutoPolicy.SetShotIntervalSeconds(ShotInterval);

	// Two shots of the hold (frames 1 and 10/11 at 60 FPS: both quantization
	// steps land on two shots within eleven frames).
	TestEqual(TEXT("the opening hold fires two shots"), Rig.Hold(11, 1.0 / 60.0), 2);
	TestEqual(TEXT("the fire book moved two rounds"), Rig.Rounds(), 28);

	// One long-stalled frame: half a second (more than three intervals) in a
	// single step.
	const double PreStallNow = Rig.Now;
	const bool bStallShot = Rig.Frame(0.5);
	TestTrue(TEXT("the stalled frame admits exactly one shot"), bStallShot);
	TestEqual(TEXT("the stall did not fire the accumulated backlog"),
		Rig.Mount->GetCommittedShotCount(), 3);
	TestEqual(TEXT("the stall deducted one round"), Rig.Rounds(), 27);

	// The same-frame double poll: the component cooldown gate refuses it named
	// and unchanged ("每帧最多一发").
	const FFireOutcome SameFrame = Rig.Fire();
	TestFalse(TEXT("the same-frame second poll fires nothing"), SameFrame.bFired);
	TestEqual(TEXT("the same-frame refusal names the cooldown"), static_cast<int32>(SameFrame.Reject),
		static_cast<int32>(EFireReject::CooldownActive));
	TestEqual(TEXT("the same-frame refusal deducts nothing"), Rig.Rounds(), 27);

	// The cadence restarts from the actual shot: the next admission opens one
	// full interval after the stall shot, not three shots' worth.
	TestEqual(TEXT("the next admission opens one interval after the stall shot"),
		Rig.Mount->GetNextFireTimeSeconds(), PreStallNow + 0.5 + ShotInterval);
	TestEqual(TEXT("the frame after the stall admits nothing early"), Rig.Hold(8, 1.0 / 60.0), 0);
	TestEqual(TEXT("the boundary frame admits the single next shot"), Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the rounds still move one per shot"), Rig.Rounds(), 26);
	return true;
}

/**
 * Acceptance: a paused fire clock freezes the cadence (no shots, no catch-up
 * after the lift) while a per-character HitStop does not exist on the fire
 * path - the gun keeps the World-clock cadence untouched ("暂停不推进冷却，
 * 单角色HitStop不改变枪冷却").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_023PauseAndHitStopLeaveCadence,
	"UEMMO.Tasks.M5_023.PauseAndHitStopLeaveCadence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_023PauseAndHitStopLeaveCadence::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}
	Rig.AutoPolicy.SetShotIntervalSeconds(ShotInterval);

	// The opening shot.
	TestEqual(TEXT("the first poll fires"), Rig.Hold(1, 1.0 / 60.0), 1);

	// Pause: the World fire clock freezes - frames keep polling but the clock
	// does not move, so no shot fires and nothing is deducted.
	for (int32 Index = 0; Index < 10; ++Index)
	{
		TestFalse(TEXT("a paused poll never fires"), Rig.Poll());
	}
	TestEqual(TEXT("the pause produced no shots"), Rig.Mount->GetCommittedShotCount(), 1);
	TestEqual(TEXT("the pause deducted nothing"), Rig.Rounds(), 29);

	// Lift: the cadence resumes from where the clock stopped - exactly one
	// shot at the interval boundary, no accumulated catch-up.
	TestEqual(TEXT("the frames before the boundary stay quiet"), Rig.Hold(8, 1.0 / 60.0), 0);
	TestEqual(TEXT("the boundary admits exactly one shot"), Rig.Hold(2, 1.0 / 60.0), 1);
	TestEqual(TEXT("the lift deducted one round"), Rig.Rounds(), 28);

	// HitStop: the character freezes while the World fire clock keeps running
	// (the fire path never reads a character clock), so the cadence stays on
	// schedule: a 60-frame hold at 60 FPS with a hitstopped character fires
	// the same boundary band as an unhindered one.
	int32 Fired = 0;
	for (int32 Index = 0; Index < 60; ++Index)
	{
		// The character clock stands still (hitstop); the fire clock advances.
		if (Rig.Frame(1.0 / 60.0))
		{
			++Fired;
		}
	}
	int32 BandMin;
	int32 BandMax;
	Rig.CountBand(60, 1.0 / 60.0, ShotInterval, BandMin, BandMax);
	TestTrue(FString::Printf(TEXT("the hitstopped hold stays inside [%d..%d]"), BandMin, BandMax),
		Fired >= BandMin && Fired <= BandMax);
	double WorstGap;
	TestTrue(TEXT("the hitstop leaves the shot spacing untouched"), Rig.SpacingHolds(ShotInterval, WorstGap));
	TestEqual(TEXT("the hitstopped hold moved one round per shot"), Rig.Rounds(), 28 - Fired);
	return true;
}

/**
 * Acceptance: releasing the trigger stops the stream and leaves no residue -
 * no stray shot and no deduction after the release, a quick tap fires exactly
 * one shot, and an open reload window terminates the stream through the
 * component's named gate without a double deduction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_023ReleaseStopsStreamNoResidue,
	"UEMMO.Tasks.M5_023.ReleaseStopsStreamNoResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_023ReleaseStopsStreamNoResidue::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}
	Rig.AutoPolicy.SetShotIntervalSeconds(ShotInterval);

	// The hold fires three shots in 25 frames (step 9 or 10 quantization both
	// land on 3).
	const int32 HoldShots = Rig.Hold(25, 1.0 / 60.0);
	TestEqual(TEXT("the hold fires three shots"), HoldShots, 3);
	const int32 CountAfterHold = Rig.Mount->GetCommittedShotCount();

	// Release: the trigger is up - frames pass without a poll, nothing fires,
	// nothing moves ("松开后不残留").
	Rig.Idle(15, 1.0 / 60.0);
	TestEqual(TEXT("the release produced no shots"), Rig.Mount->GetCommittedShotCount(), CountAfterHold);
	TestEqual(TEXT("the release deducted nothing"), Rig.Rounds(), 30 - HoldShots);

	// A quick tap fires exactly one shot; the following polls stay quiet until
	// a full interval has passed (no residue burst from the released hold).
	TestTrue(TEXT("the tap fires one shot"), Rig.Poll());
	TestFalse(TEXT("the immediate second poll stays quiet"), Rig.Poll());
	TestEqual(TEXT("the tap added exactly one shot"), Rig.Mount->GetCommittedShotCount(), CountAfterHold + 1);

	// Reload terminates the stream: while the window is open every poll is the
	// named Reloading refusal with zero movement; the completion refills the
	// book once and the stream resumes.
	const int32 RoundsBeforeReload = Rig.Rounds();
	const FAmmoReloadOutcome Open = Rig.Mount->BeginReload(Rig.Now, 0.5);
	TestTrue(TEXT("the reload window opens"), Open.bSuccess);
	Rig.Hold(5, 1.0 / 60.0);
	TestEqual(TEXT("the reload window produced no shots"), Rig.Mount->GetCommittedShotCount(), CountAfterHold + 1);
	const FAmmoReloadOutcome Close = Rig.Mount->CompleteReload();
	TestTrue(TEXT("the reload completes"), Close.bSuccess);
	TestEqual(TEXT("the completion refilled the fire book to full"), Rig.Rounds(), 30);
	TestEqual(TEXT("the completion moved no shots"), Rig.Mount->GetCommittedShotCount(), CountAfterHold + 1);
	TestTrue(TEXT("the refilled book holds the pre-reload rounds plus the granted transfer"),
		Rig.Rounds() >= RoundsBeforeReload);
	return true;
}

/**
 * Acceptance: empty magazines and admission failures stay named and
 * side-effect free under the automatic stream - the projectile-mode candidate
 * is refused by the policy, a refusing policy probe changes nothing, the empty
 * fire book is the explicit MagazineEmpty, and restoring the policy resumes
 * the stream without bypassing the atomic commit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_023RefusalsStayExplicitAndAtomic,
	"UEMMO.Tasks.M5_023.RefusalsStayExplicitAndAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_023RefusalsStayExplicitAndAtomic::RunTest(const FString& Parameters)
{
	FFireRig Rig;
	FString RigError;
	if (!TestTrue(TEXT("the rig builds"), Rig.Build(RigError)))
	{
		AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
		return false;
	}
	Rig.AutoPolicy.SetShotIntervalSeconds(ShotInterval);
	const int32 BaselineCount = Rig.Mount->GetCommittedShotCount();

	// The projectile-mode bind: the automatic policy grants hitscan only, so
	// the candidate is the named ReservationFailed with zero side effects.
	FItemInstance ProjectileInstance = MakeInstance(2, ItemProjectileTest);
	TestTrue(TEXT("the projectile weapon binds"), Rig.Mount->ApplyEquippedWeapon(&ProjectileInstance).bWeaponBound);
	Rig.RangedInstance = ProjectileInstance;
	const FFireOutcome ModeRefusal = Rig.Fire();
	TestFalse(TEXT("the projectile candidate fires nothing"), ModeRefusal.bFired);
	TestEqual(TEXT("the mode mismatch names the reservation refusal"), static_cast<int32>(ModeRefusal.Reject),
		static_cast<int32>(EFireReject::ReservationFailed));
	TestTrue(TEXT("the mode refusal names the hitscan-only grant"),
		ModeRefusal.RejectDetail.Contains(TEXT("hitscan")));
	TestEqual(TEXT("the mode refusal moved no shots"), Rig.Mount->GetCommittedShotCount(), BaselineCount);
	TestEqual(TEXT("the projectile fire book is untouched"), Rig.Rounds(), 6);

	// Back to the automatic weapon: the stream resumes (the rebind restarted
	// the local sequence book with the new generation).
	FItemInstance AutoInstance = MakeInstance(3, ItemAutoTest);
	TestTrue(TEXT("the automatic weapon rebinds"), Rig.Mount->ApplyEquippedWeapon(&AutoInstance).bWeaponBound);
	Rig.RangedInstance = AutoInstance;
	Rig.AutoPolicy.SetShotIntervalSeconds(0.0);
	TestTrue(TEXT("the stream resumes after the rebind"), Rig.Poll());

	// A refusing policy probe: named refusal, zero side effects, and the
	// automatic policy restores the stream. The clock jumps one full cooldown
	// first so the probe reaches the policy instead of the cooldown gate.
	FRefusingTestPolicy Refuser;
	Rig.Mount->SetFirePolicy(&Refuser);
	const int32 CountBeforeProbe = Rig.Mount->GetCommittedShotCount();
	Rig.Now += 0.2;
	const FFireOutcome Probe = Rig.Fire();
	TestFalse(TEXT("the refusing policy fires nothing"), Probe.bFired);
	TestEqual(TEXT("the probe names the reservation refusal"), static_cast<int32>(Probe.Reject),
		static_cast<int32>(EFireReject::ReservationFailed));
	TestEqual(TEXT("the probe moved no shots"), Rig.Mount->GetCommittedShotCount(), CountBeforeProbe);
	Rig.Mount->SetFirePolicy(&Rig.AutoPolicy);
	TestTrue(TEXT("the restored policy resumes the stream"), Rig.Poll());

	// Burn the fire book dry: the 30-round magazine empties exactly with the
	// commit count and the dry poll is the explicit MagazineEmpty.
	Rig.Hold(320, 1.0 / 60.0);
	TestEqual(TEXT("the fire book is empty"), Rig.Rounds(), 0);
	const int32 CountWhenDry = Rig.Mount->GetCommittedShotCount();
	const FFireOutcome Dry = Rig.Fire();
	TestFalse(TEXT("the dry poll fires nothing"), Dry.bFired);
	TestEqual(TEXT("the dry poll names the empty magazine"), static_cast<int32>(Dry.Reject),
		static_cast<int32>(EFireReject::MagazineEmpty));
	TestEqual(TEXT("the dry poll moved no shots"), Rig.Mount->GetCommittedShotCount(), CountWhenDry);
	TestEqual(TEXT("no shot bypassed the atomic commit"), Rig.Mount->GetCommittedShotCount(),
		Rig.ShotTimes.Num());
	return true;
}

/**
 * Acceptance: a positive policy interval is the stricter pacer (0.3 s over the
 * component's 0.15 s cooldown), and interval 0 defers the pacing to the
 * component cooldown alone - both stay inside the atomic commit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_023PolicyIntervalIsTheStricterPacer,
	"UEMMO.Tasks.M5_023.PolicyIntervalIsTheStricterPacer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_023PolicyIntervalIsTheStricterPacer::RunTest(const FString& Parameters)
{
	// The stricter policy interval: 0.3 s quantizes to an 18/19-frame step at
	// 60 FPS - both land on exactly 7 shots in 120 frames.
	{
		FFireRig Rig;
		FString RigError;
		if (!TestTrue(TEXT("the stricter rig builds"), Rig.Build(RigError)))
		{
			AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
			return false;
		}
		Rig.AutoPolicy.SetShotIntervalSeconds(0.3);

		const int32 Fired = Rig.Hold(120, 1.0 / 60.0);
		TestEqual(TEXT("the 0.3 s interval fires exactly seven shots in two seconds"), Fired, 7);
		double WorstGap;
		TestTrue(TEXT("the spacing never undercuts the policy interval"),
			Rig.SpacingHolds(0.3, WorstGap));
		TestEqual(TEXT("the fire book moved one round per shot"), Rig.Rounds(), 30 - Fired);
	}

	// Interval 0 defers to the component cooldown (60/400 RPM = 0.15 s): the
	// same boundary band as the configured-interval hold.
	{
		FFireRig Rig;
		FString RigError;
		if (!TestTrue(TEXT("the deferred rig builds"), Rig.Build(RigError)))
		{
			AddError(FString::Printf(TEXT("rig error: %s"), *RigError));
			return false;
		}
		Rig.AutoPolicy.SetShotIntervalSeconds(0.0);

		const int32 Fired = Rig.Hold(120, 1.0 / 60.0);
		int32 BandMin;
		int32 BandMax;
		Rig.CountBand(120, 1.0 / 60.0, ShotInterval, BandMin, BandMax);
		TestTrue(FString::Printf(TEXT("the deferred pacing stays inside [%d..%d]"), BandMin, BandMax),
			Fired >= BandMin && Fired <= BandMax);
		double WorstGap;
		TestTrue(TEXT("the deferred spacing never undercuts the component cooldown"),
			Rig.SpacingHolds(ShotInterval, WorstGap));
	}
	return true;
}

#endif
