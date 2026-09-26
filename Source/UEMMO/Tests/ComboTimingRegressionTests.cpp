// M1-038: frame-rate and stutter timing regression tests.
//
// The M1-037 full-combo scenario skeleton (temp world, a real UCombatComponent
// attacker, a real ATrainingEnemy, the public input path only) is replayed
// with the SAME deterministic intent sequence under four different time
// discretizations:
//
//   1. 30 fps  - one TickCombat per 1/30 s delta (two 60 Hz logic steps per
//                tick).
//   2. 60 fps  - one TickCombat per 1/60 s delta (the M1-037 baseline).
//   3. 120 fps - one TickCombat per 1/120 s delta (one 60 Hz logic step every
//                other tick).
//   4. stutter - 60 fps deltas with exactly one Delta = 0.2 s frame inserted
//                while light_01 runs on its third pre-tick frame, i.e. before
//                light_01's cancel window [12,24) opens. 200 ms = 12 logic
//                steps, more than the FCombatClock cap of 8 steps per Advance
//                call, so the backlog must survive and drain across the
//                following Advance calls without losing a window or
//                duplicating a hit.
//
// "Same seed" concept: the rig has no random factors anywhere; the fixed
// intent sequence itself is the seed. The fixed sequence (identical for every
// scenario, read from the real definitions, never hardcoded):
//
//   press 1  Light   at the first tick (Free start -> light_01)
//   press 2  Light   delivered on the first tick whose pre-tick snapshot
//                    shows light_01 at or past frame 11 (window start 12 - 1)
//                    -> chains light_02 inside [12,24)
//   press 3  Launcher delivered at or past light_02 frame 15 (window start
//                    16 - 1) -> chains launcher inside [16,29)
//   press 4  Jump    delivered at or past launcher frame 17 (window start
//                    18 - 1) -> jump cancel inside [18,32)
//   press 5  Light   delivered on the first Free tick after the jump request
//                    with the airborne flag set -> routes aerial_01
//
// The delivery rule ("at or past window start - 1") keeps every press inside
// the same logical cancel window in every discretization: the first clock step
// that sees the press is at or after the window's first frame. Cumulative game
// time is the sum of the per-tick deltas of that scenario, so the input game
// clock, the buffered lifetimes and the landing recovery all run on the same
// logical timeline while the physical tick count differs.
//
// The card compares damage counts and state-switch streams, never float-exact
// physics trajectories. The kinematic flight is integrated per tick with the
// real per-tick delta, so peak height and landing moment ARE recorded as
// deviation data against the 60 fps baseline (band asserted at 15 percent).
// The deviation data never relaxes the duplicate-hit guards: the HP trajectory
// must stay 100 - 10 - 14 - 18 - 12 = 90/76/58/46 in every scenario, and any
// duplicated or lost damage step fails the suite.
//
// The half-open boundary test drives light_01's cancel window [12,24) with
// single probe presses landing on the boundary frames 11/12/23/24 (one frame
// before the window, the first covered frame, the last covered frame, the
// first frame after the window) and asserts the [start, end) semantics:
// frame 11 is rejected (the press is held in the buffer, not consumed, not
// dropped) and is accepted by the window's first frame 12; frame 12 is
// accepted by the very step that lands on it; frame 23 (the last covered
// frame) is accepted; frame 24 is rejected - no chain, light_01 finishes
// naturally and the held press surfaces afterwards as the documented Free
// start of a fresh light_01.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatClock.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/CombatInputBuffer.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_038
{
	// The four attack ids of the full combo this suite drives.
	const FName M1_038_Light01Id(TEXT("light_01"));
	const FName M1_038_Light02Id(TEXT("light_02"));
	const FName M1_038_LauncherId(TEXT("launcher"));
	const FName M1_038_AerialId(TEXT("aerial_01"));

	// Remote base so the temp world can never collide with arena content or
	// with the scene bases of the other suites. X horizontal, Y depth, Z
	// height; the base is the attacker's feet origin (the box actor roots at
	// its feet). Each scenario scene is offset along X so coexisting scenes
	// inside one temp world never share a hit query volume.
	const FVector M1_038_SceneBase(98000.0, 96000.0, 600.0);
	constexpr double M1_038_SceneSpacing = 3000.0;

	// Half extent of the attacker body box; the same shape the M1-019/M1-022/
	// M1-024/M1-026 and M1-037 suites use, tall enough to sit fully inside
	// every attack hit box (offset (95,0,90), half extent (85,50,70)).
	const FVector M1_038_BodyHalfExtent(20.0, 20.0, 95.0);

	// The base frame grid: the action clock is fixed at 60 Hz regardless of
	// the tick delta, so logic frame N sits at N/60 s of the attack's own
	// timeline in every scenario. The input clocks are injected explicitly
	// once per simulated tick (interface contract section 2); TickCombat never
	// advances them itself.
	constexpr double M1_038_BaseFrameSeconds = 1.0 / 60.0;

	// Hard cap of simulated ticks for one scenario run; the 30 fps replay
	// needs about half the baseline tick count, the 120 fps replay about
	// twice the baseline (plus the stutter drain), so 900 leaves a wide
	// margin while a stalled run still fails fast.
	constexpr int32 M1_038_MaxScenarioTicks = 900;

	// Hard cap for one boundary probe run (light_01 is 26 frames plus the
	// post-window settle; 100 ticks at 60 fps is ample).
	constexpr int32 M1_038_MaxBoundaryTicks = 100;

	// The stutter card values: one 0.2 s frame (= 12 logic steps, capped at
	// 8 per Advance) inserted while light_01 is on pre-tick frame 3, so the
	// capped tick runs logic frames 4..11 (the whole rest of light_01's
	// active window [7,11) inside one tick) and ends one frame before the
	// cancel window [12,24) opens; the kept backlog then drains on the next
	// real frame and the window opens exactly on schedule.
	constexpr double M1_038_StutterSeconds = 0.2;
	constexpr int32 M1_038_StutterPreTickFrame = 3;

	// Kinematic expectations of the launched flight (g = 980 cm/s^2, launch
	// 700 cm/s): the rise peaks at v^2/(2g) = 250 cm and the whole flight
	// returns to the ground after 2 * 700/980 = 1.4286 s. The per-tick
	// semi-implicit integration error grows with the tick delta, so these
	// bands double as the delta-independent sanity bounds for every
	// discretization (30 fps integrates at about 239 cm peak / 1.37 s air
	// time, 120 fps at about 247 cm / 1.42 s).
	constexpr float M1_038_ExpectedPeakHeightCm = 250.0f;
	constexpr double M1_038_ExpectedFlightSeconds = 2.0 * 700.0 / 980.0;

	// The card's spatial deviation band: a recorded peak/landing deviation
	// beyond 15 percent of the 60 fps baseline fails; the actual values are
	// always recorded, never used to relax the duplicate-hit guards.
	constexpr double M1_038_MaxSpatialDeviationFraction = 0.15;

	// World acquisition, same order as the M1-018 through M1-037 pattern: a
	// private temp world first, the shared game world as fallback.
	static UWorld* M1_038_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_038_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_038 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_038 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Per-scene base: the shared base offset along X by the scene index.
	static FVector M1_038_SceneBaseFor(int32 SceneIndex)
	{
		return M1_038_SceneBase + FVector(M1_038_SceneSpacing * static_cast<double>(SceneIndex), 0.0, 0.0);
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_038_NewCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("the attack catalog initializes from DefaultGame.ini"), Catalog->InitializeFromConfig(Error)))
		{
			Test.AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
			return nullptr;
		}
		return Catalog;
	}

	/** The four time discretizations this suite replays the fixed sequence with. */
	enum class EM1_038_TimingKind : uint8
	{
		Fps30,
		Fps60,
		Fps120,
		Stutter200ms
	};

	static const TCHAR* M1_038_TimingName(EM1_038_TimingKind Kind)
	{
		switch (Kind)
		{
		case EM1_038_TimingKind::Fps30: return TEXT("30fps");
		case EM1_038_TimingKind::Fps60: return TEXT("60fps");
		case EM1_038_TimingKind::Fps120: return TEXT("120fps");
		default: return TEXT("stutter200ms");
		}
	}

	/**
	 * Everything one scenario run records (the card's evidence list): the
	 * queued input sequence, the attack switch stream, the deduplicated hit
	 * stream with damages and the target HP trajectory, the jump requests,
	 * the landing recovery state order, the launch/landing timeline including
	 * the kinematic air height of the floated enemy and the landing spot.
	 */
	struct FM1_038_RunRecord
	{
		// Input side: every queued press sequence in queue order.
		TArray<uint64> QueuedSequences;

		// Attack switch stream, in broadcast order.
		TArray<FName> StartedIds;
		TArray<FName> FinishedIds;

		// Accepted (deduplicated) hit stream, in confirm order.
		TArray<FName> HitIds;
		TArray<float> HitDamages;
		TArray<float> HitAirZ;
		TArray<float> HpAfterHit;

		int32 JumpRequests = 0;

		// Landing recovery state order: 0 = Knockdown, 1 = Recovering,
		// 2 = recovered to Free (expected exactly {0, 1, 2}).
		TArray<int32> RecoverySequence;
		bool bKnockdownSeen = false;
		bool bRecoveringSeen = false;
		bool bRecoveredToFree = false;

		// Flight timeline of the floated enemy.
		bool bLaunched = false;
		float LaunchZVelocity = 0.0f;
		double LaunchAtSeconds = -1.0;
		double LandAtSeconds = -1.0;
		float MaxAirHeightCm = 0.0f;
		FVector LandingLocation = FVector::ZeroVector;

		// True once the run actually consumed a 0.2 s stutter frame.
		bool bStutterFrameSeen = false;

		// One line per scenario event; printed through AddInfo so the
		// automation report carries the actual event order.
		FString EventOrder;
	};

	/**
	 * One scenario scene: the real catalog, the attacker box actor with its
	 * real combat component (feet provider pinned to the scene base, the
	 * injectable air-state flag standing in for the owner's IsFalling, and a
	 * counting jump request handler), and one real training enemy standing on
	 * the shared hit box center with its own combat component ticking per
	 * tick. Every run binds a fresh record; the delegate lambdas read through
	 * the scene's Record pointer.
	 */
	struct FM1_038_ScenarioScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Light01Def = nullptr;
		const UAttackDefinition* Light02Def = nullptr;
		const UAttackDefinition* LauncherDef = nullptr;
		const UAttackDefinition* AerialDef = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		UCombatComponent* EnemyCombat = nullptr;
		UCharacterMovementComponent* EnemyMovement = nullptr;
		FVector Base = FVector::ZeroVector;
		FVector BoxCenter = FVector::ZeroVector;
		FM1_038_RunRecord* Record = nullptr;
		bool bAirborneOwner = false;
		int32 JumpRequestCount = 0;
		int32 CurrentTick = 0;

		// Integrated air height of the enemy while it floats (the temp world
		// never runs physics, so the flight is integrated per tick from the
		// real vertical speed, the real pending-launch entry and the live
		// gravity, using this tick's real delta). Zero while grounded.
		float AirHeightCm = 0.0f;

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy ? Enemy->GetHealthComponent() : nullptr;
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// Builds catalog + attacker + enemy at InBase and asserts the
		// definition values the fixed schedule rides on (the design initial
		// values of the interface contract section 3 rows, plus the two chain
		// allowances the combo needs). Returns false after reporting the
		// problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			Base = InBase;
			Catalog = M1_038_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}

			Light01Def = Catalog->Find(M1_038_Light01Id);
			Light02Def = Catalog->Find(M1_038_Light02Id);
			LauncherDef = Catalog->Find(M1_038_LauncherId);
			AerialDef = Catalog->Find(M1_038_AerialId);
			if (!Test.TestTrue(TEXT("the catalog holds all four combo attacks"),
				Light01Def != nullptr && Light02Def != nullptr && LauncherDef != nullptr && AerialDef != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("light_01 keeps the design values (duration 26, active [7,11), cancel [12,24), damage 10)"),
				Light01Def->DurationFrames == 26
				&& Light01Def->ActiveWindow.StartFrame == 7 && Light01Def->ActiveWindow.EndFrame == 11
				&& Light01Def->CancelWindow.StartFrame == 12 && Light01Def->CancelWindow.EndFrame == 24
				&& FMath::IsNearlyEqual(Light01Def->BaseDamage, 10.0f)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("light_02 keeps the design values (duration 32, active [9,14), cancel [16,29), damage 14)"),
				Light02Def->DurationFrames == 32
				&& Light02Def->ActiveWindow.StartFrame == 9 && Light02Def->ActiveWindow.EndFrame == 14
				&& Light02Def->CancelWindow.StartFrame == 16 && Light02Def->CancelWindow.EndFrame == 29
				&& FMath::IsNearlyEqual(Light02Def->BaseDamage, 14.0f)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("launcher keeps the design values (duration 40, active [12,17), cancel [18,32), damage 18, launch 700)"),
				LauncherDef->DurationFrames == 40
				&& LauncherDef->ActiveWindow.StartFrame == 12 && LauncherDef->ActiveWindow.EndFrame == 17
				&& LauncherDef->CancelWindow.StartFrame == 18 && LauncherDef->CancelWindow.EndFrame == 32
				&& FMath::IsNearlyEqual(LauncherDef->BaseDamage, 18.0f)
				&& FMath::IsNearlyEqual(LauncherDef->LaunchSpeed, 700.0f)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("aerial_01 keeps the design values (duration 28, active [6,10), cancel [12,23), damage 12, launch 60)"),
				AerialDef->DurationFrames == 28
				&& AerialDef->ActiveWindow.StartFrame == 6 && AerialDef->ActiveWindow.EndFrame == 10
				&& AerialDef->CancelWindow.StartFrame == 12 && AerialDef->CancelWindow.EndFrame == 23
				&& FMath::IsNearlyEqual(AerialDef->BaseDamage, 12.0f)
				&& FMath::IsNearlyEqual(AerialDef->LaunchSpeed, 60.0f)))
			{
				return false;
			}
			// The combo schedule needs both chain allowances as data, and one
			// enemy anchor serves the whole combo only when all four attacks
			// share the same hit box geometry at the scene base.
			if (!Test.TestTrue(TEXT("light_01 allows light_02 and light_02 allows launcher as data-driven follow-ups"),
				Light01Def->AllowedNextAttacks.Contains(M1_038_Light02Id)
				&& Light02Def->AllowedNextAttacks.Contains(M1_038_LauncherId)))
			{
				return false;
			}
			const FCombatHitBox Light01Box = ComputeHitBox(Base, 1, *Light01Def);
			if (!Test.TestTrue(TEXT("all four attacks share the same hit box center at the scene base"),
				ComputeHitBox(Base, 1, *Light02Def).Center.Equals(Light01Box.Center, 1e-4f)
				&& ComputeHitBox(Base, 1, *LauncherDef).Center.Equals(Light01Box.Center, 1e-4f)
				&& ComputeHitBox(Base, 1, *AerialDef).Center.Equals(Light01Box.Center, 1e-4f)))
			{
				return false;
			}
			BoxCenter = Light01Box.Center;

			// The attacker: a plain actor rooted on a query-enabled box (the
			// M1-022 pattern) carrying the real combat component.
			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), Base, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M1_038_Body"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M1_038_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(Base);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M1_038_Combat"));
			Combat->RegisterComponent();
			if (!Test.TestTrue(TEXT("the attacker combat component accepts the catalog"), Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->SetFeetLocationProvider([this]()
			{
				return Base;
			});
			Combat->SetAirStateProvider([this]()
			{
				return bAirborneOwner;
			});
			Combat->SetJumpRequestHandler([this]()
			{
				++JumpRequestCount;
			});
			Combat->OnStarted.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				if (Record == nullptr)
				{
					return;
				}
				Record->StartedIds.Add(AttackId);
				Record->EventOrder += FString::Printf(TEXT("tick %d: Started %s #%llu\n"), CurrentTick, *AttackId.ToString(), InstanceId);
				UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: Started %s #%llu"), CurrentTick, *AttackId.ToString(), InstanceId);
			});
			Combat->OnFinished.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				if (Record == nullptr)
				{
					return;
				}
				Record->FinishedIds.Add(AttackId);
				Record->EventOrder += FString::Printf(TEXT("tick %d: Finished %s #%llu\n"), CurrentTick, *AttackId.ToString(), InstanceId);
				UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: Finished %s #%llu"), CurrentTick, *AttackId.ToString(), InstanceId);
			});
			Combat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				if (Record == nullptr)
				{
					return;
				}
				Record->HitIds.Add(Hit.AttackId);
				Record->HitDamages.Add(Hit.Damage);
				Record->HitAirZ.Add(EnemyMovement ? EnemyMovement->Velocity.Z : 0.0f);
				Record->HpAfterHit.Add(EnemyHealth());
				if (Hit.AttackId == M1_038_AerialId && Enemy != nullptr)
				{
					Record->EventOrder += FString::Printf(TEXT("tick %d: Hit %s #%llu damage %.0f hp %.0f (float cycle %d)\n"),
						CurrentTick, *Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage,
						Record->HpAfterHit.Last(), Enemy->GetLauncherCycleCount());
					UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: Hit %s #%llu damage %.0f hp %.0f"),
						CurrentTick, *Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage, Record->HpAfterHit.Last());
				}
				else
				{
					Record->EventOrder += FString::Printf(TEXT("tick %d: Hit %s #%llu damage %.0f hp %.0f\n"),
						CurrentTick, *Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage, Record->HpAfterHit.Last());
					UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: Hit %s #%llu damage %.0f hp %.0f"),
						CurrentTick, *Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage, Record->HpAfterHit.Last());
				}
			});

			// The enemy: the real training dummy, capsule center on the shared
			// hit box center. Temp worlds skip the engine's component
			// initialization, so the M1-022 activation steps are mirrored by
			// hand; ResetEnemy then pins the fresh room state (full HP,
			// Walking, zero velocity, cleared counters).
			FActorSpawnParameters SpawnParams;
			Enemy = World.SpawnActor<ATrainingEnemy>(ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, SpawnParams);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}
			if (!Enemy->HasActorBegunPlay())
			{
				Enemy->DispatchBeginPlay();
			}
			Enemy->SetSpawnAnchor(BoxCenter, FRotator::ZeroRotator);
			EnemyMovement = Enemy->GetCharacterMovement();
			if (EnemyMovement != nullptr)
			{
				if (!EnemyMovement->IsActive())
				{
					EnemyMovement->Activate(/*bReset*/ true);
				}
				if (EnemyMovement->MovementMode == MOVE_None)
				{
					EnemyMovement->SetDefaultMovementMode();
				}
			}
			EnemyCombat = Enemy->GetCombatComponent();
			if (!Test.TestTrue(TEXT("the training enemy carries its own combat component"), EnemyCombat != nullptr))
			{
				return false;
			}
			Enemy->ResetEnemy();
			return Test.TestTrue(TEXT("the fresh enemy stands alive at full health on the ground"),
				Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()
				&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f)
				&& Enemy->GetAirState() == ECombatAirState::Grounded
				&& EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		}

		// One enemy movement update without a world tick: the two real
		// pending-velocity steps (ApplyAccumulatedForces, HandlePendingLaunch
		// - the M1-022 pattern; a launch that applies flips the mode to
		// Falling itself) followed by a semi-implicit kinematic integration
		// that stands in for the physics the temp world never runs. The
		// integration uses THIS tick's real delta, so the recorded peak and
		// landing numbers are the actual discretization data the card asks to
		// record. Crossing back through the ground plane lands the enemy
		// through the real NotifyLanded dispatch (the M1-026 testable entry).
		void StepEnemyFlight(double NowSeconds, double DeltaSeconds)
		{
			if (EnemyMovement == nullptr)
			{
				return;
			}
			const float Delta = static_cast<float>(DeltaSeconds);
			EnemyMovement->ApplyAccumulatedForces(Delta);
			EnemyMovement->HandlePendingLaunch();
			if (EnemyMovement->MovementMode != MOVE_Falling)
			{
				AirHeightCm = 0.0f;
				return;
			}
			if (Record != nullptr && !Record->bLaunched)
			{
				Record->bLaunched = true;
				Record->LaunchZVelocity = EnemyMovement->Velocity.Z;
				Record->LaunchAtSeconds = NowSeconds;
				Record->EventOrder += FString::Printf(TEXT("tick %d: launched at t=%.4f vz=%.1f\n"), CurrentTick, NowSeconds, EnemyMovement->Velocity.Z);
				UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: launched at t=%.4f vz=%.1f"), CurrentTick, NowSeconds, EnemyMovement->Velocity.Z);
			}
			const float Gravity = -EnemyMovement->GetGravityZ();
			EnemyMovement->Velocity.Z -= Gravity * Delta;
			AirHeightCm += EnemyMovement->Velocity.Z * Delta;
			if (Record != nullptr)
			{
				Record->MaxAirHeightCm = FMath::Max(Record->MaxAirHeightCm, AirHeightCm);
			}
			if (AirHeightCm <= 0.0f && EnemyMovement->Velocity.Z < 0.0f)
			{
				EnemyMovement->Velocity = FVector::ZeroVector;
				EnemyMovement->SetMovementMode(MOVE_Walking);
				AirHeightCm = 0.0f;
				if (Record != nullptr)
				{
					Record->LandAtSeconds = NowSeconds;
					Record->LandingLocation = Enemy ? Enemy->GetActorLocation() : FVector::ZeroVector;
					Record->EventOrder += FString::Printf(TEXT("tick %d: landed at t=%.4f after %.3fs air time, peak %.1f cm\n"),
						CurrentTick, NowSeconds, NowSeconds - Record->LaunchAtSeconds, Record->MaxAirHeightCm);
					UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: landed at t=%.4f, peak %.1f cm"),
						CurrentTick, NowSeconds, Record->MaxAirHeightCm);
				}
				Enemy->NotifyLanded(NowSeconds);
			}
		}
	};

	/**
	 * Drives one scenario run through the public input path with the fixed
	 * intent sequence and fills the record. The per-tick delta comes from the
	 * timing kind; the press schedule observes the running attack's pre-tick
	 * snapshot and delivers each follow-up press on the first tick at or past
	 * (window start - 1), so the press is held by the buffer no later than
	 * the boundary right before the window's first frame in every
	 * discretization (all window frames are read from the definitions, never
	 * hardcoded). InOutNow carries the game clock across calls. Returns false
	 * after reporting when the scenario did not settle within the tick cap.
	 */
	static bool M1_038_RunScenario(FAutomationTestBase& Test, FM1_038_ScenarioScene& Scene, FM1_038_RunRecord& Record,
		EM1_038_TimingKind Kind, double& InOutNow)
	{
		Scene.Record = &Record;
		Scene.bAirborneOwner = false;
		Scene.JumpRequestCount = 0;

		// Delivery thresholds: window start - 1 (the last logic frame before
		// each window opens), read from the real definitions.
		const int32 Light01ChainThreshold = Scene.Light01Def->CancelWindow.StartFrame - 1;
		const int32 Light02ChainThreshold = Scene.Light02Def->CancelWindow.StartFrame - 1;
		const int32 LauncherJumpThreshold = Scene.LauncherDef->CancelWindow.StartFrame - 1;

		uint64 NextSequence = 0;
		int32 Phase = 0; // 0 queue start light, 1 wait light_01 window, 2 confirm light_02,
		                 // 3 wait light_02 window, 4 confirm launcher, 5 wait launcher window,
		                 // 6 wait jump cancel, 7 confirm aerial, 8 idle drain
		bool bStutterInserted = false;
		double Now = InOutNow;
		bool bDone = false;

		auto QueuePress = [&](ECombatInput Action, const TCHAR* Label)
		{
			FBufferedCombatInput Input;
			Input.Sequence = ++NextSequence;
			Input.Action = Action;
			Input.PressedAt = Scene.Combat->GetInputClockSeconds();
			Scene.Combat->QueueInput(Input);
			Record.QueuedSequences.Add(Input.Sequence);
			Record.EventOrder += FString::Printf(TEXT("tick %d: press %s seq %llu at t=%.4f\n"), Scene.CurrentTick, Label, Input.Sequence, Now);
			UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: press %s seq %llu at t=%.4f"), Scene.CurrentTick, Label, Input.Sequence, Now);
		};

		for (int32 Tick = 1; Tick <= M1_038_MaxScenarioTicks && !bDone; ++Tick)
		{
			const FCombatSnapshot PreTick = Scene.Combat->GetSnapshot();

			// The per-tick game delta of this discretization. The stutter kind
			// swaps exactly one 0.2 s frame in while light_01 sits on its
			// third pre-tick frame (before the cancel window opens at 12).
			double DeltaSeconds = M1_038_BaseFrameSeconds;
			switch (Kind)
			{
			case EM1_038_TimingKind::Fps30:
				DeltaSeconds = 2.0 * M1_038_BaseFrameSeconds;
				break;
			case EM1_038_TimingKind::Fps120:
				DeltaSeconds = 0.5 * M1_038_BaseFrameSeconds;
				break;
			case EM1_038_TimingKind::Stutter200ms:
				if (!bStutterInserted
					&& PreTick.ActionState == ECombatActionState::Attacking
					&& PreTick.AttackId == M1_038_Light01Id
					&& PreTick.Frame == M1_038_StutterPreTickFrame)
				{
					DeltaSeconds = M1_038_StutterSeconds;
					bStutterInserted = true;
					Record.bStutterFrameSeen = true;
					Record.EventOrder += FString::Printf(TEXT("tick %d: STUTTER frame, delta %.3f s (light_01 pre-tick frame %d)\n"),
						Tick, DeltaSeconds, PreTick.Frame);
					UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: STUTTER frame, delta %.3f s (light_01 pre-tick frame %d)"),
						Tick, DeltaSeconds, PreTick.Frame);
				}
				break;
			default:
				break;
			}

			Now += DeltaSeconds;
			Scene.CurrentTick = Tick;
			Scene.Combat->SetInputClockSeconds(Now);
			Scene.EnemyCombat->SetInputClockSeconds(Now);

			switch (Phase)
			{
			case 0: // the first press starts light_01 from Free
				QueuePress(ECombatInput::Light, TEXT("Light"));
				Phase = 1;
				break;
			case 1: // light_01's cancel window is about to open: deliver the chain Light
				if (PreTick.ActionState == ECombatActionState::Attacking
					&& PreTick.AttackId == M1_038_Light01Id && PreTick.Frame >= Light01ChainThreshold)
				{
					QueuePress(ECombatInput::Light, TEXT("Light"));
					Phase = 2;
				}
				break;
			case 2: // the chain consumed the press: light_02 is running
				if (PreTick.AttackId == M1_038_Light02Id)
				{
					Phase = 3;
				}
				break;
			case 3: // light_02's cancel window is about to open: deliver the Launcher
				if (PreTick.ActionState == ECombatActionState::Attacking
					&& PreTick.AttackId == M1_038_Light02Id && PreTick.Frame >= Light02ChainThreshold)
				{
					QueuePress(ECombatInput::Launcher, TEXT("Launcher"));
					Phase = 4;
				}
				break;
			case 4: // the chain consumed the press: launcher is running
				if (PreTick.AttackId == M1_038_LauncherId)
				{
					Phase = 5;
				}
				break;
			case 5: // the launcher's own cancel window is about to open: deliver the Jump
				if (PreTick.ActionState == ECombatActionState::Attacking
					&& PreTick.AttackId == M1_038_LauncherId && PreTick.Frame >= LauncherJumpThreshold)
				{
					QueuePress(ECombatInput::Jump, TEXT("Jump"));
					Phase = 6;
				}
				break;
			case 6: // the jump cancel returned Free with one jump request
				if (PreTick.ActionState == ECombatActionState::Free && Scene.JumpRequestCount >= 1)
				{
					// The owner answered the jump request: airborne now, so
					// the next buffered Light must route to aerial_01.
					Scene.bAirborneOwner = true;
					QueuePress(ECombatInput::Light, TEXT("Light(air)"));
					Phase = 7;
				}
				break;
			case 7: // the airborne Light started aerial_01
				if (PreTick.AttackId == M1_038_AerialId)
				{
					Phase = 8;
				}
				break;
			default: // 8: scheduling done; keep ticking flight and recovery
				break;
			}

			Scene.Combat->TickCombat(static_cast<float>(DeltaSeconds));
			Scene.EnemyCombat->TickCombat(static_cast<float>(DeltaSeconds));
			Scene.StepEnemyFlight(Now, DeltaSeconds);

			// Landing recovery sampling on the enemy's own combat component,
			// recorded as an ordered state sequence.
			const FCombatSnapshot EnemyAfter = Scene.EnemyCombat->GetSnapshot();
			if (EnemyAfter.ActionState == ECombatActionState::Knockdown && !Record.bKnockdownSeen)
			{
				Record.bKnockdownSeen = true;
				Record.RecoverySequence.Add(0);
				Record.EventOrder += FString::Printf(TEXT("tick %d: Knockdown\n"), Scene.CurrentTick);
				UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: Knockdown"), Scene.CurrentTick);
			}
			else if (EnemyAfter.ActionState == ECombatActionState::Recovering && !Record.bRecoveringSeen)
			{
				Record.bRecoveringSeen = true;
				Record.RecoverySequence.Add(1);
				Record.EventOrder += FString::Printf(TEXT("tick %d: Recovering\n"), Scene.CurrentTick);
				UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: Recovering"), Scene.CurrentTick);
			}
			else if (EnemyAfter.ActionState == ECombatActionState::Free && Record.bKnockdownSeen
				&& Record.bRecoveringSeen && Record.bLaunched && Record.LandAtSeconds > 0.0)
			{
				if (!Record.bRecoveredToFree)
				{
					Record.RecoverySequence.Add(2);
					Record.EventOrder += FString::Printf(TEXT("tick %d: recovered to Free\n"), Scene.CurrentTick);
					UE_LOG(LogTemp, Display, TEXT("M1-038 tick %d: recovered to Free"), Scene.CurrentTick);
				}
				Record.bRecoveredToFree = true;
			}

			const FCombatSnapshot After = Scene.Combat->GetSnapshot();
			// The scenario settles once aerial_01 finished, the attacker is
			// Free and the enemy walked its recovery back to Free.
			bDone = Record.FinishedIds.Contains(M1_038_AerialId)
				&& After.ActionState == ECombatActionState::Free
				&& Record.bRecoveredToFree;
		}

		Scene.Record = nullptr;
		Record.JumpRequests = Scene.JumpRequestCount;
		InOutNow = Now;
		if (!bDone)
		{
			Test.AddError(FString::Printf(TEXT("the %s scenario did not settle within %d ticks; event order:\n%s"),
				M1_038_TimingName(Kind), M1_038_MaxScenarioTicks, *Record.EventOrder));
			return false;
		}
		return true;
	}

	// One line of spatial deviation data for the report tables.
	static FString M1_038_SpatialSummary(const TCHAR* Context, const FM1_038_RunRecord& Record)
	{
		const double AirSeconds = Record.LandAtSeconds - Record.LaunchAtSeconds;
		return FString::Printf(TEXT("%s: peak %.2f cm, air time %.4f s (launch t=%.4f, land t=%.4f), landing at (%.1f, %.1f, %.1f)"),
			Context, static_cast<double>(Record.MaxAirHeightCm), AirSeconds,
			Record.LaunchAtSeconds, Record.LandAtSeconds,
			Record.LandingLocation.X, Record.LandingLocation.Y, Record.LandingLocation.Z);
	}

	// The shared full-combo stream assertions: the four attack ids in order,
	// exactly three Finished (the launcher was jump-cancelled, never
	// finished), exactly five scheduled presses, one deduplicated hit per
	// instance with the definition damages, the exact HP trajectory, one jump
	// request, the launch physics, the kinematic flight band, the ordered
	// landing recovery and the settled end state.
	static void M1_038_AssertComboStream(FAutomationTestBase& Test, FM1_038_ScenarioScene& Scene,
		const FM1_038_RunRecord& Record, const TCHAR* Context)
	{
		const TArray<FName> ExpectedStarted = { M1_038_Light01Id, M1_038_Light02Id, M1_038_LauncherId, M1_038_AerialId };
		const TArray<FName> ExpectedFinished = { M1_038_Light01Id, M1_038_Light02Id, M1_038_AerialId };
		const TArray<float> ExpectedDamages = { 10.0f, 14.0f, 18.0f, 12.0f };
		const TArray<float> ExpectedHp = { 90.0f, 76.0f, 58.0f, 46.0f };
		const TArray<uint64> ExpectedSequences = { 1, 2, 3, 4, 5 };
		const TArray<int32> ExpectedRecovery = { 0, 1, 2 };

		Test.TestTrue(FString::Printf(TEXT("%s: the scenario scheduled exactly the fixed five presses (1..5)"), Context),
			Record.QueuedSequences == ExpectedSequences);
		Test.TestTrue(FString::Printf(TEXT("%s: the combo traversed light_01, light_02, launcher, aerial_01 in order"), Context),
			Record.StartedIds == ExpectedStarted);
		Test.TestTrue(FString::Printf(TEXT("%s: exactly three attacks finished and the jump-cancelled launcher never did"), Context),
			Record.FinishedIds == ExpectedFinished);
		Test.TestTrue(FString::Printf(TEXT("%s: each of the four instances hit exactly once (deduplicated across the whole run)"), Context),
			Record.HitIds == ExpectedStarted && Record.HitDamages.Num() == 4);
		Test.TestTrue(FString::Printf(TEXT("%s: the hit damages are the definition values 10/14/18/12"), Context),
			Record.HitDamages == ExpectedDamages);
		Test.TestTrue(FString::Printf(TEXT("%s: the HP trajectory is 100-10-14-18-12 = 90/76/58/46 (no lost or duplicated damage)"), Context),
			Record.HpAfterHit == ExpectedHp);
		Test.TestEqual(FString::Printf(TEXT("%s: the jump cancel requested exactly one owner jump"), Context), Record.JumpRequests, 1);
		Test.TestTrue(FString::Printf(TEXT("%s: the landing recovery ran Knockdown, Recovering, Free in that order"), Context),
			Record.RecoverySequence == ExpectedRecovery);

		// Launch physics: the launcher applied a 700 cm/s vertical launch and
		// the floated enemy left the ground.
		Test.TestTrue(FString::Printf(TEXT("%s: the launcher hit launched the enemy at the definition speed"), Context),
			Record.bLaunched && FMath::IsNearlyEqual(Record.LaunchZVelocity, Scene.LauncherDef->LaunchSpeed, 0.01f));
		Test.TestTrue(FString::Printf(TEXT("%s: the aerial follow-up hit a floating target high above the 60 cm/s compensation floor"), Context),
			Record.HitAirZ.Num() == 4 && Record.HitAirZ[3] > 60.0f);

		// Kinematic flight band (delta-independent sanity bound; the actual
		// per-scenario numbers are recorded as deviation data).
		Test.TestTrue(FString::Printf(TEXT("%s: the float peaked near the kinematic 250 cm (got %.1f)"), Context, Record.MaxAirHeightCm),
			Record.MaxAirHeightCm > M1_038_ExpectedPeakHeightCm - 20.0f
			&& Record.MaxAirHeightCm < M1_038_ExpectedPeakHeightCm + 20.0f);
		const double FlightSeconds = Record.LandAtSeconds - Record.LaunchAtSeconds;
		Test.TestTrue(FString::Printf(TEXT("%s: the flight lasted near the kinematic %.3f s (got %.3f)"), Context, M1_038_ExpectedFlightSeconds, FlightSeconds),
			FlightSeconds > M1_038_ExpectedFlightSeconds - 0.1 && FlightSeconds < M1_038_ExpectedFlightSeconds + 0.1);

		// Settled end state: full stream left the enemy grounded on 46 HP with
		// the float cycle reopened by the recovery, and the attacker Free.
		Test.TestTrue(FString::Printf(TEXT("%s: the enemy ended grounded, Free and on 46 HP"), Context),
			Scene.Enemy->GetAirState() == ECombatAirState::Grounded
			&& Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free
			&& !Scene.EnemyCombat->IsInLandingRecovery()
			&& FMath::IsNearlyEqual(Scene.EnemyHealth(), 46.0f));
		Test.TestTrue(FString::Printf(TEXT("%s: the completed recovery reopened the float cycle and the air combo counted launcher + aerial"), Context),
			Scene.Enemy->GetLauncherCycleCount() == 0 && Scene.Enemy->GetAirComboCount() == 2);
		Test.TestTrue(FString::Printf(TEXT("%s: the attacker ended Free with an empty buffer"), Context),
			Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free
			&& Scene.Combat->GetSnapshot().BufferSize == 0
			&& Scene.Combat->GetSnapshot().InstanceId == 0);
	}

	// Cross-scenario stream equality (the card's "same damage counts and
	// state-switch streams" acceptance): compares everything the fixed
	// sequence must produce identically, deliberately excluding instance ids
	// (session counters keep minting across scenes) and tick indices (the
	// whole point of the discretization change).
	static void M1_038_AssertStreamsMatch(FAutomationTestBase& Test, const FM1_038_RunRecord& Baseline,
		const FM1_038_RunRecord& Candidate, const TCHAR* CandidateName)
	{
	const FString Context = FString::Printf(TEXT("%s vs the 60fps baseline"), CandidateName);
	Test.TestTrue(Context + TEXT(": the fixed press schedule is identical (an extra or missing delivery would show here)"),
		Candidate.QueuedSequences == Baseline.QueuedSequences);
	Test.TestTrue(Context + TEXT(": the attack switch stream (Started/Finished) is identical"),
		Candidate.StartedIds == Baseline.StartedIds && Candidate.FinishedIds == Baseline.FinishedIds);
		Test.TestTrue(Context + TEXT(": the deduplicated hit stream and damages are identical"),
			Candidate.HitIds == Baseline.HitIds && Candidate.HitDamages == Baseline.HitDamages);
		Test.TestTrue(Context + TEXT(": the HP trajectory is identical (no lost or duplicated damage)"),
			Candidate.HpAfterHit == Baseline.HpAfterHit);
		Test.TestEqual(Context + TEXT(": the jump request count is identical"), Candidate.JumpRequests, Baseline.JumpRequests);
		Test.TestTrue(Context + TEXT(": the ordered landing recovery sequence is identical"),
			Candidate.RecoverySequence == Baseline.RecoverySequence);
	}

	// The card's spatial deviation band: peak height and landing moment must
	// stay within 15 percent of the 60 fps baseline; the actual numbers are
	// always printed for the report table.
	static void M1_038_AssertSpatialDeviationBand(FAutomationTestBase& Test, const FM1_038_RunRecord& Baseline,
		const FM1_038_RunRecord& Candidate, const TCHAR* CandidateName)
	{
		const double BaselinePeak = static_cast<double>(Baseline.MaxAirHeightCm);
		const double CandidatePeak = static_cast<double>(Candidate.MaxAirHeightCm);
		const double BaselineAir = Baseline.LandAtSeconds - Baseline.LaunchAtSeconds;
		const double CandidateAir = Candidate.LandAtSeconds - Candidate.LaunchAtSeconds;
		const double PeakDeviationCm = FMath::Abs(CandidatePeak - BaselinePeak);
		const double AirDeviationSeconds = FMath::Abs(CandidateAir - BaselineAir);

		Test.AddInfo(M1_038_SpatialSummary(TEXT("baseline 60fps"), Baseline));
		Test.AddInfo(M1_038_SpatialSummary(CandidateName, Candidate));
		Test.AddInfo(FString::Printf(TEXT("%s deviation: peak %.2f cm, air time %.4f s (recorded data; band 15 percent)"),
			CandidateName, PeakDeviationCm, AirDeviationSeconds));

		Test.TestTrue(FString::Printf(TEXT("%s: the float peak deviates %.2f cm from the 60fps baseline %.2f cm, within the 15 percent band"),
			CandidateName, PeakDeviationCm, BaselinePeak),
			PeakDeviationCm < M1_038_MaxSpatialDeviationFraction * BaselinePeak);
		Test.TestTrue(FString::Printf(TEXT("%s: the landing moment deviates %.4f s from the 60fps baseline %.4f s, within the 15 percent band"),
			CandidateName, AirDeviationSeconds, BaselineAir),
			AirDeviationSeconds < M1_038_MaxSpatialDeviationFraction * BaselineAir);
	}

	/**
	 * One boundary probe scene: attacker only (no enemy - the probe targets
	 * the cancel-window semantics, not the hit pipeline), catalog values for
	 * light_01 asserted, feet provider pinned, no air provider (reads
	 * grounded) and no jump handler (no Jump is ever pressed here).
	 */
	struct FM1_038_BoundaryScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Light01Def = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		FVector Base = FVector::ZeroVector;
		FM1_038_RunRecord* StreamRecord = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			Base = InBase;
			Catalog = M1_038_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Light01Def = Catalog->Find(M1_038_Light01Id);
			if (!Test.TestTrue(TEXT("the catalog holds light_01 with the design cancel window [12,24)"),
				Light01Def != nullptr && Light01Def->DurationFrames == 26
				&& Light01Def->CancelWindow.StartFrame == 12 && Light01Def->CancelWindow.EndFrame == 24))
			{
				return false;
			}

			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), Base, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the boundary attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M1_038_BoundaryBody"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M1_038_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(Base);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M1_038_BoundaryCombat"));
			Combat->RegisterComponent();
			if (!Test.TestTrue(TEXT("the boundary combat component accepts the catalog"), Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->SetFeetLocationProvider([this]()
			{
				return Base;
			});
			// The switch streams ride on the same record shape as the combo
			// scenarios so the probe assertions read the familiar arrays.
			Combat->OnStarted.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				if (StreamRecord != nullptr)
				{
					StreamRecord->StartedIds.Add(AttackId);
					StreamRecord->EventOrder += FString::Printf(TEXT("Started %s #%llu\n"), *AttackId.ToString(), InstanceId);
				}
			});
			Combat->OnFinished.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				if (StreamRecord != nullptr)
				{
					StreamRecord->FinishedIds.Add(AttackId);
					StreamRecord->EventOrder += FString::Printf(TEXT("Finished %s #%llu\n"), *AttackId.ToString(), InstanceId);
				}
			});
			return true;
		}
	};

	/** One boundary probe observation set. */
	struct FM1_038_BoundaryRecord
	{
		bool bProbeQueued = false;
		int32 ProbeTick = -1;

		// Post-tick observations right after the probe landed on its frame.
		bool bStillLight01AfterProbe = false;
		int32 BufferAfterProbe = -1;

		// Chain observation (post-tick light_02 running).
		bool bLight02Running = false;
		int32 ChainTick = -1;
		int32 ChainPreTickFrame = -1;
		int32 BufferAfterChainTick = -1;

		// Post-window observations (frame 24 case).
		int32 Light01FinishCount = 0;
		int32 Light01StartCount = 0;
		int32 Light02StartCount = 0;

		FString EventOrder;
	};

	/**
	 * Runs one boundary probe: starts light_01 directly (the start path is
	 * not under test; the cancel-window boundary is), injects the clock per
	 * 1/60 s tick and queues exactly one Light probe press so that it lands
	 * on logical frame PressFrame (queued on the tick whose pre-tick snapshot
	 * shows frame PressFrame - 1; the probe frame is then the first step that
	 * evaluates the press). Settles when light_02 is running (in-window
	 * probes) or when the held press surfaced as the Free start of a second
	 * light_01 (the frame 24 probe).
	 */
	static bool M1_038_RunBoundaryProbe(FAutomationTestBase& Test, FM1_038_BoundaryScene& Scene,
		int32 PressFrame, FM1_038_BoundaryRecord& Record)
	{
		FM1_038_RunRecord Streams;
		Scene.StreamRecord = &Streams;

		if (!Test.TestTrue(FString::Printf(TEXT("boundary frame %d: light_01 starts directly for the probe run"), PressFrame),
			Scene.Combat->TryStartAttack(M1_038_Light01Id, 1)))
		{
			return false;
		}

		double Now = 0.0;
		bool bDone = false;
		for (int32 Tick = 1; Tick <= M1_038_MaxBoundaryTicks && !bDone; ++Tick)
		{
			Now += M1_038_BaseFrameSeconds;
			Scene.Combat->SetInputClockSeconds(Now);
			const FCombatSnapshot PreTick = Scene.Combat->GetSnapshot();

			if (!Record.bProbeQueued
				&& PreTick.ActionState == ECombatActionState::Attacking
				&& PreTick.AttackId == M1_038_Light01Id
				&& PreTick.Frame == PressFrame - 1)
			{
				FBufferedCombatInput Input;
				Input.Sequence = 1;
				Input.Action = ECombatInput::Light;
				Input.PressedAt = Scene.Combat->GetInputClockSeconds();
				Scene.Combat->QueueInput(Input);
				Record.bProbeQueued = true;
				Record.ProbeTick = Tick;
				Record.EventOrder += FString::Printf(TEXT("tick %d: probe Light queued for frame %d (pre-tick frame %d)\n"),
					Tick, PressFrame, PreTick.Frame);
			}

			Scene.Combat->TickCombat(static_cast<float>(M1_038_BaseFrameSeconds));

			const FCombatSnapshot Post = Scene.Combat->GetSnapshot();
			Record.Light01StartCount = 0;
			for (const FName StartedId : Streams.StartedIds)
			{
				if (StartedId == M1_038_Light01Id)
				{
					++Record.Light01StartCount;
				}
			}
			Record.Light02StartCount = 0;
			for (const FName StartedId : Streams.StartedIds)
			{
				if (StartedId == M1_038_Light02Id)
				{
					++Record.Light02StartCount;
				}
			}
			Record.Light01FinishCount = 0;
			for (const FName FinishedId : Streams.FinishedIds)
			{
				if (FinishedId == M1_038_Light01Id)
				{
					++Record.Light01FinishCount;
				}
			}
			Record.EventOrder += FString::Printf(TEXT("tick %d: pre frame %d -> post %s frame %d buffer %d\n"),
				Tick, PreTick.Frame, *Post.AttackId.ToString(), Post.Frame, Post.BufferSize);

			if (Record.bProbeQueued && Record.ProbeTick == Tick)
			{
				Record.bStillLight01AfterProbe = (Post.ActionState == ECombatActionState::Attacking
					&& Post.AttackId == M1_038_Light01Id);
				Record.BufferAfterProbe = Post.BufferSize;
			}
			if (!Record.bLight02Running
				&& Post.ActionState == ECombatActionState::Attacking
				&& Post.AttackId == M1_038_Light02Id)
			{
				Record.bLight02Running = true;
				Record.ChainTick = Tick;
				Record.ChainPreTickFrame = PreTick.Frame;
				Record.BufferAfterChainTick = Post.BufferSize;
			}

			if (PressFrame < 24)
			{
				bDone = Record.bLight02Running;
			}
			else
			{
				bDone = Record.Light01FinishCount >= 1
					&& Record.Light01StartCount >= 2
					&& Post.BufferSize == 0;
			}
		}

		Scene.StreamRecord = nullptr;
		if (!bDone)
		{
			Test.AddError(FString::Printf(TEXT("the boundary probe for frame %d did not settle within %d ticks; observations:\n%s"),
				PressFrame, M1_038_MaxBoundaryTicks, *Record.EventOrder));
			return false;
		}
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M1_038;

// Acceptance 1: the fixed intent sequence replayed at 30, 60 and 120 fps
// produces the identical damage counts, state-switch streams, HP trajectory,
// jump count and ordered landing recovery. The 60 fps run is the baseline;
// the 30/120 fps runs must match it stream by stream. Spatial deviation data
// against the baseline is recorded (and band-checked at 15 percent).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_038SameSequenceAcross30And60And120Fps,
	"UEMMO.Tasks.M1_038.SameSequenceAcross30And60And120Fps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_038SameSequenceAcross30And60And120Fps::RunTest(const FString& Parameters)
{
	UWorld* World = M1_038_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}

	const EM1_038_TimingKind Kinds[3] = { EM1_038_TimingKind::Fps30, EM1_038_TimingKind::Fps60, EM1_038_TimingKind::Fps120 };
	FM1_038_RunRecord Records[3];
	FM1_038_ScenarioScene Scenes[3];

	for (int32 Scenario = 0; Scenario < 3; ++Scenario)
	{
		if (!Scenes[Scenario].Build(*this, *World, M1_038_SceneBaseFor(Scenario)))
		{
			return true;
		}
		double Now = 0.0;
		if (!M1_038_RunScenario(*this, Scenes[Scenario], Records[Scenario], Kinds[Scenario], Now))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("%s event order:\n%s"), M1_038_TimingName(Kinds[Scenario]), *Records[Scenario].EventOrder));
		M1_038_AssertComboStream(*this, Scenes[Scenario], Records[Scenario], M1_038_TimingName(Kinds[Scenario]));
	}

	for (int32 Scenario = 0; Scenario < 3; ++Scenario)
	{
		if (Scenario == 1)
		{
			continue; // the 60 fps baseline compares against itself
		}
		M1_038_AssertStreamsMatch(*this, Records[1], Records[Scenario], M1_038_TimingName(Kinds[Scenario]));
		M1_038_AssertSpatialDeviationBand(*this, Records[1], Records[Scenario], M1_038_TimingName(Kinds[Scenario]));
	}

	AddInfo(TEXT("All three discretizations settled with identical streams; spatial deviation rows are printed above."));
	return true;
}

// Acceptance 2: a 200 ms stutter frame inserted before light_01's cancel
// window must not lose a window and must not duplicate a hit. The clock-level
// half proves the FCombatClock semantics directly (at most 8 steps per
// Advance; the 12 steps of 200 ms drain across two Advance calls with no time
// lost); the scenario half replays the fixed sequence with the stutter and
// compares every stream against a fresh 60 fps baseline run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_038StutterDrainsBacklogAndDoesNotDuplicate,
	"UEMMO.Tasks.M1_038.StutterDrainsBacklogAndDoesNotDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_038StutterDrainsBacklogAndDoesNotDuplicate::RunTest(const FString& Parameters)
{
	// Clock-level drain proof (the card's named mechanism).
	FCombatClock Clock;
	const int32 StutterSteps = Clock.Advance(M1_038_StutterSeconds, /*bFrozen*/ false);
	TestTrue(FString::Printf(TEXT("the 200 ms stutter frame is capped at 8 of its 12 logic steps (got %d); the backlog is kept, not lost"), StutterSteps),
		StutterSteps == 8);
	const int32 BacklogSteps = Clock.Advance(0.0, /*bFrozen*/ false);
	TestTrue(FString::Printf(TEXT("the next Advance drains the kept backlog's remaining 4 steps (got %d); 8 + 4 = the 12 steps of 200 ms"), BacklogSteps),
		BacklogSteps == 4);
	TestTrue(TEXT("the backlog is fully drained: a further empty Advance runs no step"),
		Clock.Advance(0.0, /*bFrozen*/ false) == 0);

	FCombatClock RealisticClock;
	const int32 FirstSteps = RealisticClock.Advance(M1_038_StutterSeconds, /*bFrozen*/ false);
	const int32 SecondSteps = RealisticClock.Advance(M1_038_BaseFrameSeconds, /*bFrozen*/ false);
	const int32 ThirdSteps = RealisticClock.Advance(M1_038_BaseFrameSeconds, /*bFrozen*/ false);
	TestTrue(FString::Printf(TEXT("realistic drain: the stutter's first Advance runs 8 steps (got %d)"), FirstSteps), FirstSteps == 8);
	TestTrue(FString::Printf(TEXT("realistic drain: the next real frame drains the 4-step backlog plus its own step (got %d)"), SecondSteps),
		SecondSteps == 5);
	TestTrue(FString::Printf(TEXT("realistic drain: with the backlog empty the following real frame steps exactly once (got %d)"), ThirdSteps),
		ThirdSteps == 1);
	TestTrue(FString::Printf(TEXT("realistic drain: no time lost across the two-call drain (8 + 5 + 1 = %d = the 14 steps of 200 ms + two 1/60 s frames)"),
		FirstSteps + SecondSteps + ThirdSteps), FirstSteps + SecondSteps + ThirdSteps == 14);

	UWorld* World = M1_038_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}

	// Fresh 60 fps baseline scene, then the stutter scene at its own base.
	FM1_038_ScenarioScene BaselineScene;
	if (!BaselineScene.Build(*this, *World, M1_038_SceneBaseFor(0)))
	{
		return true;
	}
	FM1_038_RunRecord BaselineRecord;
	double BaselineNow = 0.0;
	if (!M1_038_RunScenario(*this, BaselineScene, BaselineRecord, EM1_038_TimingKind::Fps60, BaselineNow))
	{
		return true;
	}
	AddInfo(FString::Printf(TEXT("60fps baseline event order:\n%s"), *BaselineRecord.EventOrder));

	FM1_038_ScenarioScene StutterScene;
	if (!StutterScene.Build(*this, *World, M1_038_SceneBaseFor(1)))
	{
		return true;
	}
	FM1_038_RunRecord StutterRecord;
	double StutterNow = 0.0;
	if (!M1_038_RunScenario(*this, StutterScene, StutterRecord, EM1_038_TimingKind::Stutter200ms, StutterNow))
	{
		return true;
	}
	AddInfo(FString::Printf(TEXT("stutter event order:\n%s"), *StutterRecord.EventOrder));

	TestTrue(TEXT("the stutter run actually consumed its 0.2 s frame (the injection happened where scheduled)"),
		StutterRecord.bStutterFrameSeen);

	// The stutter run must satisfy the full-combo stream on its own (this is
	// where a duplicated or lost damage step, a lost window or a stall would
	// fail), then match the baseline stream by stream.
	M1_038_AssertComboStream(*this, StutterScene, StutterRecord, TEXT("stutter200ms"));
	M1_038_AssertStreamsMatch(*this, BaselineRecord, StutterRecord, TEXT("stutter200ms"));

	AddInfo(TEXT("The stutter scenario settled with the identical stream: the capped 8-step frame kept its backlog,"));
	AddInfo(TEXT("the backlog drained on the following frame and the light_01 cancel window opened without loss or duplication."));
	return true;
}

// Acceptance 3: the half-open cancel window [12,24) boundary at +-1 frame.
// Four probe runs deliver one Light press landing on logical frames 11, 12,
// 23 and 24 of running light_01 and assert the [start, end) semantics:
// frame 11 rejected (press held in the buffer, accepted by the window's first
// frame 12), frame 12 accepted (consumed by the very step that lands on the
// window start), frame 23 accepted (the last covered frame), frame 24
// rejected (no chain; light_01 finishes naturally and the held press surfaces
// as the documented Free start of a fresh light_01).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_038HalfOpenBoundaryAt11And12And23And24,
	"UEMMO.Tasks.M1_038.HalfOpenBoundaryAt11And12And23And24",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_038HalfOpenBoundaryAt11And12And23And24::RunTest(const FString& Parameters)
{
	UWorld* World = M1_038_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}

	// Frame 11 (one before the window): rejected at 11, held, accepted at 12.
	{
		FM1_038_BoundaryScene Scene;
		if (!Scene.Build(*this, *World, M1_038_SceneBaseFor(4)))
		{
			return true;
		}
		FM1_038_BoundaryRecord Record;
		if (!M1_038_RunBoundaryProbe(*this, Scene, 11, Record))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("probe frame 11 observations:\n%s"), *Record.EventOrder));
		TestTrue(TEXT("frame 11: the probe was delivered on schedule"), Record.bProbeQueued);
		TestTrue(TEXT("frame 11 is outside [12,24): the press was rejected there (light_01 still running after the frame 11 step)"),
			Record.bStillLight01AfterProbe);
		TestTrue(TEXT("frame 11: the press was neither consumed nor dropped - it is still buffered after the frame 11 step"),
			Record.BufferAfterProbe == 1);
		TestTrue(TEXT("frame 11: the held press was accepted by the window's first frame: light_02 runs afterwards"),
			Record.bLight02Running);
		TestEqual(TEXT("frame 11: the chain step is exactly frame 12 (the tick's pre-tick frame was 11)"), Record.ChainPreTickFrame, 11);
		TestEqual(TEXT("frame 11: the buffer emptied with the accepted chain"), Record.BufferAfterChainTick, 0);
	}

	// Frame 12 (the window start): accepted by that very step.
	{
		FM1_038_BoundaryScene Scene;
		if (!Scene.Build(*this, *World, M1_038_SceneBaseFor(5)))
		{
			return true;
		}
		FM1_038_BoundaryRecord Record;
		if (!M1_038_RunBoundaryProbe(*this, Scene, 12, Record))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("probe frame 12 observations:\n%s"), *Record.EventOrder));
		TestTrue(TEXT("frame 12: the probe was delivered on schedule"), Record.bProbeQueued);
		TestTrue(TEXT("frame 12 is the first frame of [12,24): the press was accepted by the step that landed on frame 12 (light_02 runs)"),
			Record.bLight02Running);
		TestEqual(TEXT("frame 12: the chain happened on the probe tick itself (delivered at pre-tick 11, consumed at 12)"),
			Record.ChainTick, Record.ProbeTick);
		TestEqual(TEXT("frame 12: the chain step is exactly the window start (the tick's pre-tick frame was 11)"), Record.ChainPreTickFrame, 11);
		TestEqual(TEXT("frame 12: the buffer emptied with the accepted chain"), Record.BufferAfterProbe, 0);
	}

	// Frame 23 (the last covered frame of [12,24)): accepted.
	{
		FM1_038_BoundaryScene Scene;
		if (!Scene.Build(*this, *World, M1_038_SceneBaseFor(6)))
		{
			return true;
		}
		FM1_038_BoundaryRecord Record;
		if (!M1_038_RunBoundaryProbe(*this, Scene, 23, Record))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("probe frame 23 observations:\n%s"), *Record.EventOrder));
		TestTrue(TEXT("frame 23: the probe was delivered on schedule"), Record.bProbeQueued);
		TestTrue(TEXT("frame 23 is the last covered frame of [12,24): the press was accepted (light_02 runs afterwards)"),
			Record.bLight02Running);
		TestEqual(TEXT("frame 23: the chain happened on the probe tick itself (delivered at pre-tick 22, consumed at 23)"),
			Record.ChainTick, Record.ProbeTick);
		TestEqual(TEXT("frame 23: the chain step is exactly the last covered frame (the tick's pre-tick frame was 22)"), Record.ChainPreTickFrame, 22);
		TestEqual(TEXT("frame 23: the buffer emptied with the accepted chain"), Record.BufferAfterProbe, 0);
	}

	// Frame 24 (the first frame after the window): rejected as a chain.
	{
		FM1_038_BoundaryScene Scene;
		if (!Scene.Build(*this, *World, M1_038_SceneBaseFor(7)))
		{
			return true;
		}
		FM1_038_BoundaryRecord Record;
		if (!M1_038_RunBoundaryProbe(*this, Scene, 24, Record))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("probe frame 24 observations:\n%s"), *Record.EventOrder));
		TestTrue(TEXT("frame 24: the probe was delivered on schedule"), Record.bProbeQueued);
		TestTrue(TEXT("frame 24 is outside [12,24): the press was rejected there (light_01 still running after the frame 24 step)"),
			Record.bStillLight01AfterProbe);
		TestTrue(TEXT("frame 24: the press was not consumed - it is still buffered after the frame 24 step"), Record.BufferAfterProbe == 1);
		TestTrue(TEXT("frame 24: no cancel-window chain ever happened (light_02 never started)"),
			!Record.bLight02Running && Record.Light02StartCount == 0);
		TestEqual(TEXT("frame 24: light_01 finished naturally exactly once"), Record.Light01FinishCount, 1);
		TestEqual(TEXT("frame 24: the held press surfaced as the documented Free start of a fresh light_01"), Record.Light01StartCount, 2);
	}

	return true;
}

// Acceptance 4: the stutter scenario's spatial deviation data against the
// 60 fps baseline, recorded (never used to relax the duplicate-hit guards -
// the HP trajectory equality is asserted again here).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_038StutterSpatialDeviationRecorded,
	"UEMMO.Tasks.M1_038.StutterSpatialDeviationRecorded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_038StutterSpatialDeviationRecorded::RunTest(const FString& Parameters)
{
	UWorld* World = M1_038_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}

	FM1_038_ScenarioScene BaselineScene;
	if (!BaselineScene.Build(*this, *World, M1_038_SceneBaseFor(0)))
	{
		return true;
	}
	FM1_038_RunRecord BaselineRecord;
	double BaselineNow = 0.0;
	if (!M1_038_RunScenario(*this, BaselineScene, BaselineRecord, EM1_038_TimingKind::Fps60, BaselineNow))
	{
		return true;
	}

	FM1_038_ScenarioScene StutterScene;
	if (!StutterScene.Build(*this, *World, M1_038_SceneBaseFor(1)))
	{
		return true;
	}
	FM1_038_RunRecord StutterRecord;
	double StutterNow = 0.0;
	if (!M1_038_RunScenario(*this, StutterScene, StutterRecord, EM1_038_TimingKind::Stutter200ms, StutterNow))
	{
		return true;
	}

	TestTrue(TEXT("the stutter run actually consumed its 0.2 s frame"), StutterRecord.bStutterFrameSeen);

	// The duplicate-hit guard stands regardless of the deviation data: the
	// stutter scenario's HP trajectory must equal the baseline's exactly.
	M1_038_AssertStreamsMatch(*this, BaselineRecord, StutterRecord, TEXT("stutter200ms"));

	// The recorded deviation data with the card's 15 percent band.
	M1_038_AssertSpatialDeviationBand(*this, BaselineRecord, StutterRecord, TEXT("stutter200ms"));

	AddInfo(TEXT("Deviation rows above are the recorded stutter-vs-60fps spatial data (peak height, landing moment, landing spot)."));
	return true;
}

#endif
