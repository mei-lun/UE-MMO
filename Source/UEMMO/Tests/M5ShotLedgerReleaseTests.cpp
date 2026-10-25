// M5-077: the production projectile/hitscan retirement paths must release
// their hit-ledger shot keys. M5-050 (Artifacts/Tasks/M5-050/stress-report.json)
// recorded the production gap this card closes: the pawn's ShotHitLedger is
// bound once (PrototypeCharacter.cpp WireWeaponMount) and the weapon delivery
// paths record every hit into it, but NOTHING ever calls EndShot on the
// retirement side - a finished pellet drops out of LiveWeaponProjectiles and a
// hitscan shot resolves inside its own commit, while the ledger keeps the shot
// instance active forever. FHitLedger bounds ACTIVE shot instances at
// DefaultCapacity = 256 and never evicts a live key, so after 256 distinct
// hitting shots every further hit is refused with
// EHitLedgerRejectReason::CapacityFull - silently (no damage, no refusal
// surface the player can see).
//
// RED -> GREEN: these cases are written against the FIXED behavior and are
// EXPECTED TO FAIL until M5-077 wires the EndShot release into the production
// retirement paths (the coordinator's production change: record each live
// pellet's FShotContext and call ShotHitLedger.EndShot(Epoch, SourceEntityId,
// ShotId) when a shot's last pellet retires, plus the hitscan-side release).
// Without the wiring the projectile case starts failing at exactly shot 257
// (the ledger fills at DefaultCapacity) and the hitscan case fails the same
// way - the first-silent-shot number in the failure message is the diagnosis.
//
// The assertions are deliberately BEHAVIOR-LEVEL only: APrototypeCharacter's
// ShotHitLedger is private, and the contract under test is not the ledger's
// bookkeeping (M5-010's own tests pin that) but the observable production
// promise "sustained fire never loses hits". Each case therefore drives >256
// real committed shots through the REAL input -> TryFire -> delivery chain of
// the M5-033 rig shape (real pawn, real UWeaponComponent bind, injected
// clocks) against one high-pool target, and asserts every single shot dealt
// exactly its damage - a CapacityFull refusal anywhere shows up as a shot
// that "silently misses" (health unchanged), never as a ledger read.
//
// Duration control: the rig's projectile is the production 3000 cm/s speed
// with a short 0.2 s lifetime and the target sits at the M5-033 400 cm lane
// distance, so every pellet hits (or expires) within ~13 frames and each shot
// waits at most 120 frames (2 s of simulated time) for full retirement;
// 300 shots finish in a few thousand simulated frames without rendering.
#include "Misc/AutomationTest.h"

#include "../PrototypeCharacter.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/HealthComponent.h"
#include "../Items/ItemDefinition.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_077
{
	// Fixed simulated frame step (60 fps like the design frame clock; the
	// M5-033 rig precedent).
	constexpr float M5_077_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content or the other M5 rigs (the M1-022/M1-027 placement convention).
	// X horizontal, Y depth, Z height; the floor top sits exactly at the
	// base Z.
	const FVector M5_077_SceneBase(170000.0, 62000.0, 600.0);

	const float M5_077_FloorHalfThickness = 100.0f;
	const float M5_077_FloorHalfExtentXY = 4000.0f;
	const float M5_077_PlayerSpawnHeight = 120.0f;

	// The target stands 400 cm ahead of the pawn (facing +X); the shot origin
	// rides the feet offset (30, 0, 50) and the trace flies straight +X, so a
	// box centered at the same height catches every pellet of every shot (the
	// exact M5-033 lane geometry; the pawn never moves and the profile
	// requests no control, so the lane stays identical across the whole run).
	const FVector M5_077_TargetOffset(400.0, 0.0, 50.0);
	const FVector M5_077_TargetBoxHalfExtent(50.0, 50.0, 60.0);

	// The run's accounting numbers: one profile, one pellet, one damage number.
	// BaseDamage 10 with AttackCoefficient 0 (a pure base-damage hit) and
	// AttackPower hard-0 in both delivery paths resolves to exactly 10 per
	// accepted hit - the per-shot pool drop is then an exact hit counter.
	constexpr float M5_077_DamagePerHit = 10.0f;
	constexpr float M5_077_DamageTolerance = 0.01f;

	// 300 shots > FHitLedger::DefaultCapacity (256): without the EndShot
	// wiring the ledger fills during shots 1..256 and shot 257 is the first
	// CapacityFull silent refusal. The 100000-point pool keeps every shot of
	// the 3000-damage run far above the death floor.
	constexpr int32 M5_077_ShotCount = 300;
	constexpr float M5_077_TargetMaxHealth = 100000.0f;

	// Per-shot retirement budget: 120 frames = 2 s of simulated time. The
	// pellet needs 400 cm / 3000 cm/s ~= 0.133 s (8 frames) to reach the
	// target and a miss expires at the 0.2 s lifetime (12 frames), so the
	// budget is ~10x the worst honest retirement and only a wedged pellet
	// can hit it.
	constexpr int32 M5_077_RetireWaitFrames = 120;

	// Weapon fixture values: the production projectile speed with a short
	// lifetime; the fire rate sits at the schema's 3000 rpm ceiling so the
	// 0.02 s cooldown always elapses during a retirement wait (the wait, not
	// the pacing, owns the run's duration). The 400-round magazine (> 999
	// bound is refused; 400 > 300 shots) needs no reload mid-run - the bind
	// starts the magazine full without touching the shared reserve.
	constexpr float M5_077_ProjectileSpeedCmS = 3000.0f;
	constexpr float M5_077_ProjectileLifetimeS = 0.2f;
	constexpr float M5_077_FireRateRpm = 3000.0f;
	constexpr int32 M5_077_MagazineSize = 400;
	constexpr int32 M5_077_ReserveStock = 1000;
	constexpr float M5_077_HitscanRangeCm = 5000.0f;

	const FName M5_077_DamageProfileId(TEXT("dmg_rig_077_round"));
	const FName M5_077_AmmoId(TEXT("ammo_rig_077_cell"));

	// World-static blocking floor box the pawn stands on (temp worlds ship no
	// geometry; the M1-041 precedent verbatim).
	static AActor* M5_077_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M5_077_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_077_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M5_077_FloorHalfExtentXY, M5_077_FloorHalfExtentXY, M5_077_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M5_077_FloorHalfThickness));
		return Actor;
	}

	// A bare hostile target: a world-static box plus a real health pool (the
	// unified entry's Stage 1 ignores bodies without one). Registered into
	// the pawn's fire registry lazily by the pawn's own target-identity
	// resolver on the first hit - exactly the production path. The pool is
	// oversized through the M5-050 target shape (raise the max, then
	// ResetHealth fills the pool - a raised max never heals by itself).
	static AActor* M5_077_SpawnTarget(UWorld& World, const FVector& Base, UHealthComponent** OutHealth)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base + M5_077_TargetOffset, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_077_TargetBody"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_077_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(Base + M5_077_TargetOffset);
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_077_TargetHealth"));
		Health->RegisterComponent();
		Health->SetMaxHealth(M5_077_TargetMaxHealth);
		Health->ResetHealth();
		if (OutHealth != nullptr)
		{
			*OutHealth = Health;
		}
		return Actor;
	}

	// One full rig: the wrapper owns the manually ticked temp world, the rig
	// owns the catalog VALUES mounted into the real pawn's weapon component,
	// and the shared test clock feeds both the pawn's input layer and the
	// mount's fire clock (deterministic pacing). Structure copied from the
	// M5-033 FM5_033_Rig (this card regression-tests that same wiring at
	// sustained-fire depth); the fixture values are M5-077's own.
	struct FM5_077_Rig
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		FCombatCatalog CatalogValue;
		FItemDefinitionCatalog ItemsValue;
		FGuid BoundInstanceId;
		double ClockSeconds = 100.0;

		bool Build(FAutomationTestBase& Test, bool bWithTarget)
		{
			if (!Test.TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			if (M5_077_SpawnFloor(*World, M5_077_SceneBase) == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();
			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				M5_077_SceneBase + FVector(0.0, 0.0, M5_077_PlayerSpawnHeight), FRotator::ZeroRotator, PlayerParams);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
			{
				Movement->bRunPhysicsWithNoController = true;
				if (!Movement->IsActive())
				{
					Movement->Activate(/*bReset*/ true);
				}
				if (Movement->MovementMode == MOVE_None)
				{
					Movement->SetDefaultMovementMode();
				}
			}
			if (bWithTarget)
			{
				Target = M5_077_SpawnTarget(*World, M5_077_SceneBase, &TargetHealth);
				if (!Test.TestNotNull(TEXT("the target actor spawns"), Target))
				{
					return false;
				}
			}
			// The shared rig clock feeds the pawn's input layer AND the mount's
			// fire clock (the M5-022 injection surface), so pacing is exact.
			Player->SetWeaponInputClockProvider([this]() { return ClockSeconds; });
			Player->GetWeaponMount()->SetFireClockProvider([this]() { return ClockSeconds; });
			return true;
		}

		// Ticks the world until the pawn settled onto the floor (the M3-031
		// settle pattern) so the feet origin and the facing are final.
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
			for (int32 Frame = 0; Frame < 300; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks (settle)"), Wrapper.TickTestWorld(M5_077_FrameSeconds)))
				{
					return false;
				}
				ClockSeconds += M5_077_FrameSeconds;
				if (Movement != nullptr && Movement->MovementMode == MOVE_Walking && Movement->Velocity.Size() < 1.0f)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the pawn never settled onto the floor"));
			return false;
		}

		// Builds a production-shaped catalog (one pure-base-damage profile,
		// the weapon, its ammo and - when referenced - its short-lived
		// projectile), mounts it into the pawn's real weapon component,
		// pre-seeds the shared reserve and binds a real item instance through
		// the production bind entry.
		bool BindWeapon(FAutomationTestBase& Test, const FWeaponDefinition& WeaponDef)
		{
			FParsedCombatConfig Parsed;
			Parsed.SchemaVersion = 1;
			FDamageProfile Profile;
			Profile.DamageProfileId = M5_077_DamageProfileId;
			Profile.BaseDamage = M5_077_DamagePerHit;
			// AttackCoefficient 0 = a pure base-damage hit (the delivery paths
			// pass AttackPower 0 anyway): the per-hit damage is exactly
			// BaseDamage, which makes the per-shot pool drop an exact hit
			// counter for the whole sustained run.
			Profile.AttackCoefficient = 0.0f;
			// All controls stay 0: the target box is never staggered,
			// knocked back or launched, so it never leaves the shot lane
			// across hundreds of shots.
			Profile.HitStunSeconds = 0.0f;
			Profile.KnockbackCmPerSecond = 0.0f;
			Profile.LaunchCmPerSecond = 0.0f;
			Profile.HitStopSeconds = 0.0f;
			Parsed.DamageProfiles.Add(Profile);
			FAmmoType Ammo;
			Ammo.AmmoId = WeaponDef.AmmoId;
			Ammo.MaxReserve = M5_077_ReserveStock;
			Ammo.MagazineSize = WeaponDef.MagazineSize;
			Parsed.AmmoTypes.Add(Ammo);
			if (!WeaponDef.ProjectileId.IsNone())
			{
				FProjectileDefinition Projectile;
				Projectile.ProjectileId = WeaponDef.ProjectileId;
				Projectile.Motion = EProjectileMotion::Straight;
				Projectile.SpeedCmS = M5_077_ProjectileSpeedCmS;
				// The short lifetime keeps every shot's retirement (hit or
				// expiry) inside the per-shot wait budget - the sustained-run
				// duration control.
				Projectile.LifetimeS = M5_077_ProjectileLifetimeS;
				Projectile.DamageProfileId = Profile.DamageProfileId;
				Projectile.PierceCount = 0;
				Parsed.Projectiles.Add(Projectile);
			}
			FWeaponDefinition Weapon = WeaponDef;
			Parsed.Weapons.Add(Weapon);
			FString BuildErrors;
			if (!Test.TestTrue(TEXT("the rig catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, CatalogValue, BuildErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog build refused: %s"), *BuildErrors));
				return false;
			}
			FItemDefinition Item;
			Item.DefinitionId = FName(TEXT("item_rig_077_rifle"));
			Item.DisplayName = TEXT("Rig Rifle 077");
			Item.Slot = EItemSlot::Weapon;
			Item.Rarity = EItemRarity::Normal;
			Item.WeaponDefinitionId = WeaponDef.WeaponId;
			FString ItemError;
			if (!Test.TestTrue(TEXT("the rig item definition registers"), ItemsValue.AddDefinition(Item, &ItemError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig item definition refused: %s"), *ItemError));
				return false;
			}
			UWeaponComponent* Mount = Player->GetWeaponMount();
			FString MountError;
			if (!Test.TestTrue(TEXT("the rig catalogs mount into the pawn's component"),
				Mount->MountCatalogs(&CatalogValue, &ItemsValue, &MountError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog mount refused: %s"), *MountError));
				return false;
			}
			// The reserve is pre-seeded so the bind's own zero-reserve
			// registration is a refused duplicate (the M5AutomaticFireTests
			// fixture pattern): the shared pool keeps its full stock. The bind
			// starts the magazine full without consuming the reserve, so the
			// whole run lives on the magazine alone.
			if (!Test.TestTrue(TEXT("the rig reserve ammo registers"),
				Mount->GetAmmoModel().RegisterAmmoType(WeaponDef.AmmoId, M5_077_ReserveStock, M5_077_ReserveStock)))
			{
				return false;
			}
			FItemInstance Instance;
			Instance.InstanceId = FGuid(0x0770B000u, 0x0BECu, 0x5777u, 1u);
			Instance.DefinitionId = Item.DefinitionId;
			Instance.RollSeed = 0x52077;
			Instance.Level = 1;
			const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
			if (!Test.TestTrue(TEXT("the rig weapon binds"), Outcome.bSucceeded && Outcome.bWeaponBound))
			{
				Test.AddError(FString::Printf(TEXT("the rig bind refused (%d): %s"),
					static_cast<int32>(Outcome.Reject), *Outcome.RejectDetail));
				return false;
			}
			BoundInstanceId = Instance.InstanceId;
			// The pawn's input-layer wiring (policy selection, gates, the
			// delivery bridge) applies exactly as the production refresh tail
			// applies it.
			Player->ApplyWeaponBindWiring();
			return Test.TestTrue(TEXT("the bound mount authorizes fire"), Mount->IsFireAuthorized());
		}

		// Advances the world and the shared rig clock one frame at a time.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Wrapper.TickTestWorld(M5_077_FrameSeconds))
				{
					break;
				}
				ClockSeconds += M5_077_FrameSeconds;
			}
		}

		int32 CommittedShots() const
		{
			return Player != nullptr && Player->GetWeaponMount() != nullptr
				? Player->GetWeaponMount()->GetCommittedShotCount()
				: 0;
		}
	};

	// The two production-shaped weapon definitions the cases bind. Both share
	// the fixture numbers; only the delivery mode differs, which is exactly
	// the surface under test (the ledger keys open through the M5-026
	// executor path AND through the M5-027/028 projectile path alike).
	static FWeaponDefinition M5_077_MakeProjectileWeaponDefinition()
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = FName(TEXT("weapon_rig_077_projectile"));
		Weapon.FireMode = EWeaponFireMode::Projectile;
		// The projectile definition owns the damage (the schema rule).
		Weapon.DamageProfileId = FName(NAME_None);
		Weapon.AmmoId = M5_077_AmmoId;
		Weapon.MagazineSize = M5_077_MagazineSize;
		Weapon.FireRateRpm = M5_077_FireRateRpm;
		Weapon.BurstCount = 1;
		Weapon.PelletCount = 1;
		Weapon.SpreadDegrees = 0.0f;
		// Projectile mode keeps the range 0 (the projectile speed/lifetime
		// own the reach).
		Weapon.RangeCm = 0.0f;
		Weapon.ProjectileId = FName(TEXT("bullet_rig_077_linear"));
		return Weapon;
	}

	static FWeaponDefinition M5_077_MakeHitscanWeaponDefinition()
	{
		FWeaponDefinition Weapon = M5_077_MakeProjectileWeaponDefinition();
		Weapon.WeaponId = FName(TEXT("weapon_rig_077_hitscan"));
		Weapon.FireMode = EWeaponFireMode::Hitscan;
		Weapon.DamageProfileId = M5_077_DamageProfileId;
		Weapon.ProjectileId = FName(NAME_None);
		Weapon.RangeCm = M5_077_HitscanRangeCm;
		return Weapon;
	}

	/** What one sustained-fire run observed (behavior-level only; the pawn's
	 * ledger is private and the contract under test is the observable one). */
	struct FM5_077_SustainedReport
	{
		/** Presses that actually committed one shot each. */
		int32 ShotsCommitted = 0;

		/** First shot whose damage was not exactly DamagePerHit (0 = none).
		 * Without the EndShot wiring the projectile and hitscan cases both
		 * report 257 here - the ledger filled at DefaultCapacity. */
		int32 FirstSilentShot = 0;

		/** Shots that did not deal exactly DamagePerHit ("silent misses"). */
		int32 SilentShotCount = 0;

		/** Shots whose pellet never retired inside the per-shot wait budget. */
		int32 RetireTimeouts = 0;

		/** The target's whole-pool drop across the run. */
		double TotalDamage = 0.0;
	};

	/**
	 * The shared sustained-fire driver of both cases: fires ShotCount real
	 * committed shots one at a time against the rig's target and accounts
	 * every shot's damage. With bWaitForPelletRetirement (the projectile
	 * mode) each shot waits until GetLiveWeaponProjectileCount() returns to
	 * 0 - the exact "the shot fully retired before the next one" pacing that
	 * keeps at most one shot in flight, so WITH the EndShot wiring the
	 * ledger's active count oscillates 0..1 and never approaches the 256
	 * capacity. The hitscan mode commits and resolves synchronously inside
	 * the press (the executor runs in the commit broadcast), so a fixed
	 * two-frame pause per shot keeps the identical one-shot-at-a-time shape.
	 * A rig-health failure (a press that did not commit, a pellet that never
	 * retired) stops the run with a named error instead of reporting an
	 * unreadable regression signal.
	 */
	static FM5_077_SustainedReport M5_077_FireSustainedShots(FAutomationTestBase& Test, FM5_077_Rig& Rig,
		int32 ShotCount, bool bWaitForPelletRetirement)
	{
		FM5_077_SustainedReport Report;
		APrototypeCharacter* Player = Rig.Player;
		for (int32 Shot = 1; Shot <= ShotCount; ++Shot)
		{
			const float HealthBefore = Rig.TargetHealth->GetHealth();
			// The pacing window rides the shared rig clock, which only moves
			// inside TickFrames - a press issued on the same clock value the
			// previous shot's retire check landed on can sit a few
			// milliseconds short of the 3000 RPM window (the M5-059 pacing
			// refusal seen on the first run). One settling tick before every
			// press keeps it strictly past the previous deadline.
			Rig.TickFrames(1);
			Player->OnFireInputPressed();
			Player->OnFireInputReleased();
			if (Rig.CommittedShots() != Shot)
			{
				Test.AddError(FString::Printf(TEXT("shot %d did not commit (the mount reports %d committed shots); the run stops"),
					Shot, Rig.CommittedShots()));
				return Report;
			}
			if (bWaitForPelletRetirement)
			{
				// The commit broadcast binds exactly one live pellet before
				// the press returns (synchronous reserve/commit/bind); a
				// different count is a rig failure, not a ledger signal.
				if (Player->GetLiveWeaponProjectileCount() != 1)
				{
					Test.AddError(FString::Printf(TEXT("shot %d did not bind exactly one live pellet (live=%d); the run stops"),
						Shot, Player->GetLiveWeaponProjectileCount()));
					return Report;
				}
				// Wait for the pellet's full retirement: the hit terminates
				// the policy (PierceCount 0) and the pawn's per-tick sweep
				// retires the body in the same frame the damage applied, so
				// the wait exiting on the live count back at 0 also proves
				// the shot's ledger-relevant life is over.
				bool bRetired = false;
				for (int32 Frame = 0; Frame < M5_077_RetireWaitFrames && !bRetired; ++Frame)
				{
					Rig.TickFrames(1);
					bRetired = Player->GetLiveWeaponProjectileCount() == 0;
				}
				if (!bRetired)
				{
					++Report.RetireTimeouts;
					Test.AddError(FString::Printf(TEXT("shot %d's pellet never retired within the %d-frame wait; the run stops"),
						Shot, M5_077_RetireWaitFrames));
					return Report;
				}
			}
			else
			{
				// The hitscan commit resolves inside the press (the executor
				// runs synchronously in the commit broadcast); a fixed pause
				// keeps the one-shot-at-a-time pacing of the projectile run.
				Rig.TickFrames(2);
			}
			// The per-shot hit accounting: an accepted hit removed exactly
			// DamagePerHit; a CapacityFull refusal (or any other silent
			// rejection) leaves the pool untouched - a "silent miss" the
			// player would never see a reason for.
			const float Drop = HealthBefore - Rig.TargetHealth->GetHealth();
			Report.TotalDamage += static_cast<double>(Drop);
			if (!FMath::IsNearlyEqual(Drop, M5_077_DamagePerHit, M5_077_DamageTolerance))
			{
				++Report.SilentShotCount;
				if (Report.FirstSilentShot == 0)
				{
					Report.FirstSilentShot = Shot;
				}
			}
			++Report.ShotsCommitted;
		}
		return Report;
	}
}

using namespace UE::UEMMO::Tasks::M5_077;

// ---------------------------------------------------------------------------
// RetiredShotsReleaseLedgerKeysBeyondCapacity
// ---------------------------------------------------------------------------

// The projectile retirement path releases its ledger keys: 300 real committed
// shots (one live pellet at a time, each fully retired before the next press)
// every deal their exact damage. Without the M5-077 EndShot wiring the pawn's
// ShotHitLedger accumulates one active instance per shot and shot 257 (the
// ledger's DefaultCapacity 256 + 1) is the first CapacityFull silent refusal -
// this case is the regression's red state by design.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_077RetiredShotsReleaseLedgerKeysBeyondCapacity,
	"UEMMO.Tasks.M5_077.RetiredShotsReleaseLedgerKeysBeyondCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_077RetiredShotsReleaseLedgerKeysBeyondCapacity::RunTest(const FString& Parameters)
{
	FM5_077_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_077_MakeProjectileWeaponDefinition()))
	{
		return true;
	}
	if (!TestNotNull(TEXT("the target carries a health pool"), Rig.TargetHealth))
	{
		return true;
	}

	const FM5_077_SustainedReport Report = M5_077_FireSustainedShots(*this, Rig,
		M5_077_ShotCount, /*bWaitForPelletRetirement*/ true);

	TestEqual(TEXT("every press committed its shot"), Report.ShotsCommitted, M5_077_ShotCount);
	TestEqual(TEXT("every shot's pellet retired inside the wait budget"), Report.RetireTimeouts, 0);
	TestTrue(FString::Printf(TEXT("all %d shots dealt their damage with no CapacityFull silent refusal (first silent shot %d, %d silent shots)"),
		M5_077_ShotCount, Report.FirstSilentShot, Report.SilentShotCount), Report.SilentShotCount == 0);
	TestTrue(FString::Printf(TEXT("the target's total drop equals shots x per-hit damage (%.1f of %.1f)"),
		Report.TotalDamage, static_cast<double>(M5_077_ShotCount) * M5_077_DamagePerHit),
		FMath::IsNearlyEqual(Report.TotalDamage, static_cast<double>(M5_077_ShotCount) * M5_077_DamagePerHit, 0.5));
	TestEqual(TEXT("no pellet stays live after the run"), Rig.Player->GetLiveWeaponProjectileCount(), 0);
	TestTrue(TEXT("the target survives the run (the pool never reached the death floor)"), Rig.TargetHealth->IsAlive());
	return true;
}

// ---------------------------------------------------------------------------
// HitscanShotsAlsoReleaseKeys
// ---------------------------------------------------------------------------

// The hitscan path needs the same release: its shots open ledger instances
// through the M5-026 executor too (ExecuteHitscan receives the pawn's
// ShotHitLedger and every hit goes through ApplyUnifiedHit), but a hitscan
// shot has no live pellet to await - its "retirement" is the commit itself
// resolving. 300 committed hitscan shots must all deal their exact damage.
// Without the wiring this fails at shot 257 exactly like the projectile case.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_077HitscanShotsAlsoReleaseKeys,
	"UEMMO.Tasks.M5_077.HitscanShotsAlsoReleaseKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_077HitscanShotsAlsoReleaseKeys::RunTest(const FString& Parameters)
{
	FM5_077_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_077_MakeHitscanWeaponDefinition()))
	{
		return true;
	}
	if (!TestNotNull(TEXT("the target carries a health pool"), Rig.TargetHealth))
	{
		return true;
	}

	const FM5_077_SustainedReport Report = M5_077_FireSustainedShots(*this, Rig,
		M5_077_ShotCount, /*bWaitForPelletRetirement*/ false);

	TestEqual(TEXT("every press committed its shot"), Report.ShotsCommitted, M5_077_ShotCount);
	TestTrue(FString::Printf(TEXT("all %d hitscan shots dealt their damage with no CapacityFull silent refusal (first silent shot %d, %d silent shots)"),
		M5_077_ShotCount, Report.FirstSilentShot, Report.SilentShotCount), Report.SilentShotCount == 0);
	TestTrue(FString::Printf(TEXT("the target's total drop equals shots x per-hit damage (%.1f of %.1f)"),
		Report.TotalDamage, static_cast<double>(M5_077_ShotCount) * M5_077_DamagePerHit),
		FMath::IsNearlyEqual(Report.TotalDamage, static_cast<double>(M5_077_ShotCount) * M5_077_DamagePerHit, 0.5));
	// A hitscan weapon never leaves live pellets - the mode's own invariant,
	// pinned here so the shared driver cannot silently blur the two modes.
	TestEqual(TEXT("the hitscan weapon leaves no live pellets"), Rig.Player->GetLiveWeaponProjectileCount(), 0);
	TestTrue(TEXT("the target survives the run (the pool never reached the death floor)"), Rig.TargetHealth->IsAlive());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
