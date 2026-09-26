// M1-037: repeatable full-combo scenario tests.
//
// One temp world, one real UCombatComponent attacker (the M1-018..M1-027 hit
// pipeline pattern: a box-root actor whose actor location is the feet origin)
// and one real ATrainingEnemy. The whole combo is driven through the public
// input path only: QueueInput with an explicitly injected input game clock and
// one TickCombat per 1/60 s frame, exactly like the earlier suites (interface
// contract section 2). No test ever writes the enemy health directly: every HP
// step comes from an accepted hit flowing through QueryTargets -> ApplyDamage
// -> NotifyHitReceived -> OnHitConfirmed.
//
// Scenario (positive): buffered Light starts light_01 from Free, a Light
// pressed inside light_01's cancel window chains light_02, a Launcher pressed
// inside light_02's window chains launcher, a Jump pressed inside the
// launcher's own window cancels into one owner jump request, the airborne
// owner's buffered Light starts aerial_01, the launched enemy floats, lands
// and runs its Knockdown -> Recovering -> Free recovery. The negative replays
// the first chain and then presses the next Light one frame after light_02's
// cancel window closed: the press is never consumed (it ages out and is
// pruned on the first Free tick), so nothing auto-chains. The ten-round test
// replays the full combo after component-level resets and compares every
// round's event and HP trajectory against the first round.

#include "Misc/AutomationTest.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
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

namespace UE::UEMMO::Tasks::M1_037
{
	// The four attack ids of the full combo this suite drives.
	const FName M1_037_Light01Id(TEXT("light_01"));
	const FName M1_037_Light02Id(TEXT("light_02"));
	const FName M1_037_LauncherId(TEXT("launcher"));
	const FName M1_037_AerialId(TEXT("aerial_01"));

	// Remote base so the temp world can never collide with arena content or
	// with the scene bases of the other suites. X horizontal, Y depth, Z
	// height; the base is the attacker's feet origin (the box actor roots at
	// its feet).
	const FVector M1_037_SceneBase(92000.0, 94000.0, 600.0);

	// Half extent of the attacker body box; the same shape the M1-019/M1-022/
	// M1-024/M1-026 suites use, tall enough to sit fully inside every attack
	// hit box (offset (95,0,90), half extent (85,50,70)).
	const FVector M1_037_BodyHalfExtent(20.0, 20.0, 95.0);

	// The tests drive both combat components with single 1/60 s steps, so
	// logic frame N sits at N/60 s of game time. The input clocks are
	// injected explicitly once per simulated frame (interface contract
	// section 2); TickCombat never advances them itself.
	constexpr double M1_037_FrameSeconds = 1.0 / 60.0;

	// Hard cap of simulated frames for one scenario run; the full combo plus
	// the enemy flight and recovery settles around 175 frames, so 600 leaves
	// a wide margin while a desynchronized run still fails fast.
	constexpr int32 M1_037_MaxScenarioTicks = 600;

	// The deliberate miss ages its press with one extra clock step so the
	// buffered lifetime (150 ms) is exhausted before the attack ends.
	constexpr double M1_037_MissAgeSeconds = 0.2;

	// Kinematic expectations of the launched flight (g = 980 cm/s^2, launch
	// 700 cm/s): the rise peaks at v^2/(2g) = 250 cm after 0.714 s and the
	// whole flight returns to the ground after 2 * 700/980 = 1.4286 s. The
	// per-frame semi-implicit integration stays within a couple of percent
	// of both, so the assertions keep a small tolerance band around them.
	constexpr float M1_037_ExpectedPeakHeightCm = 250.0f;
	constexpr double M1_037_ExpectedFlightSeconds = 2.0 * 700.0 / 980.0;

	// World acquisition, same order as the M1-018 through M1-027 pattern: a
	// private temp world first, the shared game world as fallback.
	static UWorld* M1_037_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_037_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_037 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_037 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_037_NewCatalog(FAutomationTestBase& Test)
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

	/**
	 * Everything one scenario run records (the card's evidence list): the
	 * queued input sequences, the attack switch stream (Started/Finished with
	 * instance ids), the deduplicated hit stream with damages and the target
	 * HP trajectory, the jump requests, and the launch/landing/recovery
	 * timeline including the kinematic air height of the floated enemy.
	 */
	struct FM1_037_RunRecord
	{
		// Input side: every queued press sequence in queue order.
		TArray<uint64> QueuedSequences;

		// Attack switch stream, in broadcast order.
		TArray<FName> StartedIds;
		TArray<uint64> StartedInstanceIds;
		TArray<FName> FinishedIds;
		TArray<uint64> FinishedInstanceIds;

		// Accepted (deduplicated) hit stream, in confirm order.
		TArray<FName> HitIds;
		TArray<uint64> HitInstanceIds;
		TArray<float> HitDamages;
		TArray<float> HitAirZ;
		TArray<float> HpAfterHit;

		int32 JumpRequests = 0;

		// Flight timeline of the floated enemy.
		bool bLaunched = false;
		float LaunchZVelocity = 0.0f;
		double LaunchAtSeconds = -1.0;
		double LandAtSeconds = -1.0;
		float MaxAirHeightCm = 0.0f;

		// Landing recovery observations.
		int32 FloatCycleAtAerialHit = -1;
		bool bKnockdownSeen = false;
		bool bRecoveringSeen = false;
		bool bRecoveredToFree = false;

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
	 * frame. Every run binds a fresh record; the delegate lambdas read
	 * through the scene's Record pointer.
	 */
	struct FM1_037_ScenarioScene
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
		FVector BoxCenter = FVector::ZeroVector;
		FM1_037_RunRecord* Record = nullptr;
		bool bAirborneOwner = false;
		int32 JumpRequestCount = 0;
		int32 CurrentTick = 0;

		// Integrated air height of the enemy while it floats (the temp world
		// never runs physics, so the flight is integrated per frame from the
		// real vertical speed and the live gravity). Zero while grounded.
		float AirHeightCm = 0.0f;

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy ? Enemy->GetHealthComponent() : nullptr;
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// Builds catalog + attacker + enemy and asserts the definition values
		// the scenario schedule rides on (the design initial values of the
		// interface contract section 3 rows, plus the two chain allowances the
		// combo needs). Returns false after reporting the problem so the
		// caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			Catalog = M1_037_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}

			Light01Def = Catalog->Find(M1_037_Light01Id);
			Light02Def = Catalog->Find(M1_037_Light02Id);
			LauncherDef = Catalog->Find(M1_037_LauncherId);
			AerialDef = Catalog->Find(M1_037_AerialId);
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
				Light01Def->AllowedNextAttacks.Contains(M1_037_Light02Id)
				&& Light02Def->AllowedNextAttacks.Contains(M1_037_LauncherId)))
			{
				return false;
			}
			const FCombatHitBox Light01Box = ComputeHitBox(M1_037_SceneBase, 1, *Light01Def);
			if (!Test.TestTrue(TEXT("all four attacks share the same hit box center at the scene base"),
				ComputeHitBox(M1_037_SceneBase, 1, *Light02Def).Center.Equals(Light01Box.Center, 1e-4f)
				&& ComputeHitBox(M1_037_SceneBase, 1, *LauncherDef).Center.Equals(Light01Box.Center, 1e-4f)
				&& ComputeHitBox(M1_037_SceneBase, 1, *AerialDef).Center.Equals(Light01Box.Center, 1e-4f)))
			{
				return false;
			}
			BoxCenter = Light01Box.Center;

			// The attacker: a plain actor rooted on a query-enabled box (the
			// M1-022 pattern) carrying the real combat component.
			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), M1_037_SceneBase, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M1_037_Body"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M1_037_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(M1_037_SceneBase);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M1_037_Combat"));
			Combat->RegisterComponent();
			if (!Test.TestTrue(TEXT("the attacker combat component accepts the catalog"), Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->SetFeetLocationProvider([]()
			{
				return M1_037_SceneBase;
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
				Record->StartedInstanceIds.Add(InstanceId);
				Record->EventOrder += FString::Printf(TEXT("tick %d: Started %s #%llu\n"), CurrentTick, *AttackId.ToString(), InstanceId);
				UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: Started %s #%llu"), CurrentTick, *AttackId.ToString(), InstanceId);
			});
			Combat->OnFinished.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				if (Record == nullptr)
				{
					return;
				}
				Record->FinishedIds.Add(AttackId);
				Record->FinishedInstanceIds.Add(InstanceId);
				Record->EventOrder += FString::Printf(TEXT("tick %d: Finished %s #%llu\n"), CurrentTick, *AttackId.ToString(), InstanceId);
				UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: Finished %s #%llu"), CurrentTick, *AttackId.ToString(), InstanceId);
			});
			Combat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				if (Record == nullptr)
				{
					return;
				}
				Record->HitIds.Add(Hit.AttackId);
				Record->HitInstanceIds.Add(Hit.AttackInstanceId);
				Record->HitDamages.Add(Hit.Damage);
				Record->HitAirZ.Add(EnemyMovement ? EnemyMovement->Velocity.Z : 0.0f);
				Record->HpAfterHit.Add(EnemyHealth());
				if (Hit.AttackId == M1_037_AerialId && Enemy != nullptr)
				{
					Record->FloatCycleAtAerialHit = Enemy->GetLauncherCycleCount();
				}
				Record->EventOrder += FString::Printf(TEXT("tick %d: Hit %s #%llu damage %.0f hp %.0f\n"),
					CurrentTick, *Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage, Record->HpAfterHit.Last());
				UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: Hit %s #%llu damage %.0f hp %.0f"),
					CurrentTick, *Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage, Record->HpAfterHit.Last());
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
		// integrated height rises and falls with the real vertical speed and
		// crossing back through the ground plane lands the enemy through the
		// real NotifyLanded dispatch (the M1-026 testable entry).
		void StepEnemyFlight(double NowSeconds)
		{
			if (EnemyMovement == nullptr)
			{
				return;
			}
			EnemyMovement->ApplyAccumulatedForces(static_cast<float>(M1_037_FrameSeconds));
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
				UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: launched at t=%.4f vz=%.1f"), CurrentTick, NowSeconds, EnemyMovement->Velocity.Z);
			}
			const float Gravity = -EnemyMovement->GetGravityZ();
			EnemyMovement->Velocity.Z -= Gravity * static_cast<float>(M1_037_FrameSeconds);
			AirHeightCm += EnemyMovement->Velocity.Z * static_cast<float>(M1_037_FrameSeconds);
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
					Record->EventOrder += FString::Printf(TEXT("tick %d: landed at t=%.4f after %.3fs air time, peak %.1f cm\n"),
						CurrentTick, NowSeconds, NowSeconds - Record->LaunchAtSeconds, Record->MaxAirHeightCm);
					UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: landed at t=%.4f, peak %.1f cm"), CurrentTick, NowSeconds, Record->MaxAirHeightCm);
				}
				Enemy->NotifyLanded(NowSeconds);
			}
		}
	};

	enum class EM1_037_ScenarioKind : uint8
	{
		/** The full combo: light_01, light_02, launcher, jump cancel, aerial. */
		FullCombo,
		/** The negative: light_01, light_02, then a Light pressed one frame after light_02's cancel window closed. */
		MissedWindowAfterLight02
	};

	/**
	 * Drives one scenario run through the public input path and fills the
	 * record. The press schedule is a small phase machine that observes the
	 * running attack's snapshot frame between ticks and queues each press
	 * with PressedAt = the component's injected input clock (so a press queued
	 * during any freeze would still be judged against the pinned clock value).
	 * All window frames are read from the definitions, never hardcoded.
	 * InOutNow carries the monotonic input game clock across runs (the ten
	 * round test keeps one session clock). Returns false after reporting when
	 * the scenario did not settle within the tick cap.
	 */
	static bool M1_037_RunScenario(FAutomationTestBase& Test, FM1_037_ScenarioScene& Scene, FM1_037_RunRecord& Record,
		EM1_037_ScenarioKind Kind, double& InOutNow)
	{
		Scene.Record = &Record;
		Scene.bAirborneOwner = false;
		Scene.JumpRequestCount = 0;
		const bool bNegative = (Kind == EM1_037_ScenarioKind::MissedWindowAfterLight02);

		// Schedule frames, all read from the real definitions: the first frame
		// of each cancel window queues the follow-up press (consumed one frame
		// later, still inside the half-open window), and the negative miss
		// queues at EndFrame, the first frame after light_02's window.
		const int32 Light01ChainFrame = Scene.Light01Def->CancelWindow.StartFrame;
		const int32 Light02ChainFrame = Scene.Light02Def->CancelWindow.StartFrame;
		const int32 LauncherJumpFrame = Scene.LauncherDef->CancelWindow.StartFrame;
		const int32 MissFrame = Scene.Light02Def->CancelWindow.EndFrame;

		uint64 NextSequence = 0;
		int32 Phase = 0; // 0 queue start light, 1 wait light_01 window, 2 confirm light_02,
		                 // 3 wait light_02 window, 4 confirm launcher, 5 wait launcher window,
		                 // 6 wait jump cancel, 7 confirm aerial, 8 idle/negative drain
		bool bAgePendingClock = false;
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
			UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: press %s seq %llu at t=%.4f"), Scene.CurrentTick, Label, Input.Sequence, Now);
		};

		for (int32 Tick = 1; Tick <= M1_037_MaxScenarioTicks && !bDone; ++Tick)
		{
			Now += M1_037_FrameSeconds;
			if (bAgePendingClock)
			{
				// The deliberate miss ages its press past the 150 ms buffered
				// lifetime before the running attack can end.
				Now += M1_037_MissAgeSeconds;
				bAgePendingClock = false;
			}
			Scene.CurrentTick = Tick;
			Scene.Combat->SetInputClockSeconds(Now);
			Scene.EnemyCombat->SetInputClockSeconds(Now);

			const FCombatSnapshot Before = Scene.Combat->GetSnapshot();
			switch (Phase)
			{
			case 0: // the first press starts light_01 from Free
				QueuePress(ECombatInput::Light, TEXT("Light"));
				Phase = 1;
				break;
			case 1: // light_01's cancel window opens: queue the chain Light
				if (Before.ActionState == ECombatActionState::Attacking
					&& Before.AttackId == M1_037_Light01Id && Before.Frame == Light01ChainFrame)
				{
					QueuePress(ECombatInput::Light, TEXT("Light"));
					Phase = 2;
				}
				break;
			case 2: // the chain consumed the press: light_02 is running
				if (Before.AttackId == M1_037_Light02Id)
				{
					Phase = 3;
				}
				break;
			case 3: // light_02's cancel window
				if (Before.AttackId == M1_037_Light02Id)
				{
					if (bNegative && Before.Frame == MissFrame)
					{
						// One frame after [16,29) closed: the press must never
						// chain; it ages out and is pruned while Free.
						QueuePress(ECombatInput::Light, TEXT("Light(miss)"));
						bAgePendingClock = true;
						Phase = 8;
					}
					else if (!bNegative && Before.Frame == Light02ChainFrame)
					{
						QueuePress(ECombatInput::Launcher, TEXT("Launcher"));
						Phase = 4;
					}
				}
				break;
			case 4: // the chain consumed the press: launcher is running
				if (Before.AttackId == M1_037_LauncherId)
				{
					Phase = 5;
				}
				break;
			case 5: // the launcher's own cancel window: queue the jump cancel
				if (Before.AttackId == M1_037_LauncherId && Before.Frame == LauncherJumpFrame)
				{
					QueuePress(ECombatInput::Jump, TEXT("Jump"));
					Phase = 6;
				}
				break;
			case 6: // the jump cancel returned Free with one jump request
				if (Before.ActionState == ECombatActionState::Free && Scene.JumpRequestCount >= 1)
				{
					// The owner answered the jump request: airborne now, so
					// the next buffered Light must route to aerial_01.
					Scene.bAirborneOwner = true;
					QueuePress(ECombatInput::Light, TEXT("Light(air)"));
					Phase = 7;
				}
				break;
			case 7: // the airborne Light started aerial_01
				if (Before.AttackId == M1_037_AerialId)
				{
					Phase = 8;
				}
				break;
			default: // 8: scheduling done; keep ticking flight and recovery
				break;
			}

			Scene.Combat->TickCombat(static_cast<float>(M1_037_FrameSeconds));
			Scene.EnemyCombat->TickCombat(static_cast<float>(M1_037_FrameSeconds));
			Scene.StepEnemyFlight(Now);

			// Landing recovery sampling on the enemy's own combat component.
			const FCombatSnapshot EnemyAfter = Scene.EnemyCombat->GetSnapshot();
			if (EnemyAfter.ActionState == ECombatActionState::Knockdown && !Record.bKnockdownSeen)
			{
				Record.bKnockdownSeen = true;
				Record.EventOrder += FString::Printf(TEXT("tick %d: Knockdown\n"), Scene.CurrentTick);
				UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: Knockdown"), Scene.CurrentTick);
			}
			else if (EnemyAfter.ActionState == ECombatActionState::Recovering && !Record.bRecoveringSeen)
			{
				Record.bRecoveringSeen = true;
				Record.EventOrder += FString::Printf(TEXT("tick %d: Recovering\n"), Scene.CurrentTick);
				UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: Recovering"), Scene.CurrentTick);
			}
			else if (EnemyAfter.ActionState == ECombatActionState::Free && Record.bKnockdownSeen
				&& Record.bRecoveringSeen && Record.bLaunched && Record.LandAtSeconds > 0.0)
			{
				if (!Record.bRecoveredToFree)
				{
					Record.EventOrder += FString::Printf(TEXT("tick %d: recovered to Free\n"), Scene.CurrentTick);
					UE_LOG(LogTemp, Display, TEXT("M1-037 tick %d: recovered to Free"), Scene.CurrentTick);
				}
				Record.bRecoveredToFree = true;
			}

			const FCombatSnapshot After = Scene.Combat->GetSnapshot();
			if (bNegative)
			{
				// The negative settles once light_02 ended naturally and the
				// aged miss press was pruned on the first Free tick.
				bDone = Record.FinishedIds.Contains(M1_037_Light02Id)
					&& After.ActionState == ECombatActionState::Free
					&& After.BufferSize == 0;
			}
			else
			{
				// The positive settles once aerial_01 finished, the attacker
				// is Free and the enemy walked its recovery back to Free.
				bDone = Record.FinishedIds.Contains(M1_037_AerialId)
					&& After.ActionState == ECombatActionState::Free
					&& Record.bRecoveredToFree;
			}
		}

		Scene.Record = nullptr;
		Record.JumpRequests = Scene.JumpRequestCount;
		InOutNow = Now;
		if (!bDone)
		{
			Test.AddError(FString::Printf(TEXT("the scenario did not settle within %d ticks; event order:\n%s"),
				M1_037_MaxScenarioTicks, *Record.EventOrder));
			return false;
		}
		return true;
	}

	// Counts the world's actors (leak check across rounds).
	static void M1_037_CountActors(UWorld& World, int32& OutTotal, int32& OutEnemies)
	{
		OutTotal = 0;
		OutEnemies = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			++OutTotal;
		}
		for (TActorIterator<ATrainingEnemy> It(&World); It; ++It)
		{
			++OutEnemies;
		}
	}

	// The shared full-combo stream assertions (used by the single positive
	// run and by every round of the ten-round replay): the four attack ids in
	// order, exactly three Finished (the launcher was jump-cancelled, never
	// finished), one deduplicated hit per instance with the definition
	// damages, the exact HP trajectory, one jump request, the launch physics,
	// the kinematic flight, the landing recovery and the settled end state.
	static void M1_037_AssertFullComboStream(FAutomationTestBase& Test, FM1_037_ScenarioScene& Scene,
		const FM1_037_RunRecord& Record, const TCHAR* Context)
	{
		const TArray<FName> ExpectedStarted = { M1_037_Light01Id, M1_037_Light02Id, M1_037_LauncherId, M1_037_AerialId };
		const TArray<FName> ExpectedFinished = { M1_037_Light01Id, M1_037_Light02Id, M1_037_AerialId };
		const TArray<float> ExpectedDamages = { 10.0f, 14.0f, 18.0f, 12.0f };
		const TArray<float> ExpectedHp = { 90.0f, 76.0f, 58.0f, 46.0f };

		Test.TestTrue(FString::Printf(TEXT("%s: the combo traversed light_01, light_02, launcher, aerial_01 in order"), Context),
			Record.StartedIds == ExpectedStarted);
		Test.TestTrue(FString::Printf(TEXT("%s: exactly three attacks finished and the jump-cancelled launcher never did"), Context),
			Record.FinishedIds == ExpectedFinished && Record.FinishedIds.Contains(M1_037_LauncherId) == false);
		Test.TestTrue(FString::Printf(TEXT("%s: each of the four instances hit exactly once (deduplicated)"), Context),
			Record.HitIds == ExpectedStarted && Record.HitDamages.Num() == 4);
		Test.TestTrue(FString::Printf(TEXT("%s: the hit damages are the definition values 10/14/18/12"), Context),
			Record.HitDamages == ExpectedDamages);
		Test.TestTrue(FString::Printf(TEXT("%s: the HP trajectory is 100-10-14-18-12 = 90/76/58/46"), Context),
			Record.HpAfterHit == ExpectedHp);
		Test.TestEqual(FString::Printf(TEXT("%s: the jump cancel requested exactly one owner jump"), Context), Record.JumpRequests, 1);

		// Launch physics: the launcher applied a 700 cm/s vertical launch and
		// the floated enemy left the ground.
		Test.TestTrue(FString::Printf(TEXT("%s: the launcher hit launched the enemy at the definition speed"), Context),
			Record.bLaunched && FMath::IsNearlyEqual(Record.LaunchZVelocity, Scene.LauncherDef->LaunchSpeed, 0.01f));
		Test.TestTrue(FString::Printf(TEXT("%s: the aerial follow-up hit a floating target high above the 60 cm/s compensation floor"), Context),
			Record.HitAirZ.Num() == 4 && Record.HitAirZ[3] > 60.0f);
		Test.TestEqual(FString::Printf(TEXT("%s: the aerial hit saw the launcher's float cycle open (1)"), Context),
			Record.FloatCycleAtAerialHit, 1);

		// Kinematic flight: the rise peaks near v^2/(2g) = 250 cm and the
		// flight lands near 2v/g = 1.43 s after the launch.
		Test.TestTrue(FString::Printf(TEXT("%s: the float peaked near the kinematic 250 cm (got %.1f)"), Context, Record.MaxAirHeightCm),
			Record.MaxAirHeightCm > M1_037_ExpectedPeakHeightCm - 20.0f
			&& Record.MaxAirHeightCm < M1_037_ExpectedPeakHeightCm + 20.0f);
		const double FlightSeconds = Record.LandAtSeconds - Record.LaunchAtSeconds;
		Test.TestTrue(FString::Printf(TEXT("%s: the flight lasted near the kinematic %.3f s (got %.3f)"), Context, M1_037_ExpectedFlightSeconds, FlightSeconds),
			FlightSeconds > M1_037_ExpectedFlightSeconds - 0.1 && FlightSeconds < M1_037_ExpectedFlightSeconds + 0.1);

		// Landing recovery: one Knockdown -> Recovering -> Free process.
		Test.TestTrue(FString::Printf(TEXT("%s: the launched landing ran Knockdown, Recovering and returned to Free"), Context),
			Record.bKnockdownSeen && Record.bRecoveringSeen && Record.bRecoveredToFree);

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
}

using namespace UE::UEMMO::Tasks::M1_037;

// Core acceptance: the full combo driven purely through QueueInput/TickCombat
// traverses all four attacks, floats and lands the enemy through the real hit
// pipeline, and the recorded streams (input sequences, attack switches,
// deduplicated hits, HP trajectory, flight and recovery) match the design.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_037FullComboScenarioTraversesAllFourAttacks,
	"UEMMO.Tasks.M1_037.FullComboScenarioTraversesAllFourAttacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_037FullComboScenarioTraversesAllFourAttacks::RunTest(const FString& Parameters)
{
	UWorld* World = M1_037_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_037_ScenarioScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	FM1_037_RunRecord Record;
	double Now = 0.0;
	if (!M1_037_RunScenario(*this, Scene, Record, EM1_037_ScenarioKind::FullCombo, Now))
	{
		return true;
	}
	AddInfo(TEXT("Full combo event order:\n") + Record.EventOrder);

	// The press schedule queued exactly five presses: start Light, chain
	// Light, chain Launcher, cancel Jump and the airborne Light.
	const TArray<uint64> ExpectedSequences = { 1, 2, 3, 4, 5 };
	TestTrue(TEXT("the scenario queued exactly five timestamped presses (1..5)"),
		Record.QueuedSequences == ExpectedSequences);

	// Instance ids of a fresh session: one monotonic id per started attack.
	const TArray<uint64> ExpectedInstances = { 1, 2, 3, 4 };
	TestTrue(TEXT("the four started attacks minted the fresh session instance ids 1..4"),
		Record.StartedInstanceIds == ExpectedInstances);
	TestTrue(TEXT("the hits came from the same four instances"),
		Record.HitInstanceIds == ExpectedInstances);

	M1_037_AssertFullComboStream(*this, Scene, Record, TEXT("full combo"));
	return true;
}

// Negative acceptance: a Light pressed one frame after light_02's cancel
// window closed must not auto-chain. The miss press ages out in the buffer,
// light_02 ends naturally, nothing new starts and the HP trajectory stops at
// the two chained light hits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_037MissedWindowNegativeDoesNotAutoChain,
	"UEMMO.Tasks.M1_037.MissedWindowNegativeDoesNotAutoChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_037MissedWindowNegativeDoesNotAutoChain::RunTest(const FString& Parameters)
{
	UWorld* World = M1_037_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_037_ScenarioScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	FM1_037_RunRecord Record;
	double Now = 0.0;
	if (!M1_037_RunScenario(*this, Scene, Record, EM1_037_ScenarioKind::MissedWindowAfterLight02, Now))
	{
		return true;
	}
	AddInfo(TEXT("Missed-window negative event order:\n") + Record.EventOrder);

	const TArray<FName> ExpectedStarted = { M1_037_Light01Id, M1_037_Light02Id };
	const TArray<FName> ExpectedFinished = { M1_037_Light01Id, M1_037_Light02Id };
	const TArray<float> ExpectedDamages = { 10.0f, 14.0f };
	const TArray<float> ExpectedHp = { 90.0f, 76.0f };

	TestTrue(TEXT("only the in-window chain light_01 -> light_02 started; the missed press started nothing"),
		Record.StartedIds == ExpectedStarted);
	TestTrue(TEXT("light_01 and light_02 both ended naturally and nothing else ever finished"),
		Record.FinishedIds == ExpectedFinished);
	TestTrue(TEXT("the two chained lights hit once each for 10 and 14 damage"),
		Record.HitIds == ExpectedStarted && Record.HitDamages == ExpectedDamages);
	TestTrue(TEXT("the HP trajectory stopped at 90/76 (the missed window did not add damage)"),
		Record.HpAfterHit == ExpectedHp && FMath::IsNearlyEqual(Scene.EnemyHealth(), 76.0f));
	TestEqual(TEXT("no jump was ever requested (the miss press was a Light, and nothing consumed it)"),
		Record.JumpRequests, 0);
	TestTrue(TEXT("the missed press never launched the enemy: the dummy stayed grounded the whole run"),
		!Record.bLaunched && Record.LandAtSeconds < 0.0
		&& Scene.Enemy->GetAirState() == ECombatAirState::Grounded
		&& !Scene.EnemyCombat->IsInLandingRecovery());
	TestTrue(TEXT("the aged miss press was pruned, not consumed: the attacker ended Free with an empty buffer"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free
		&& Scene.Combat->GetSnapshot().BufferSize == 0
		&& Scene.Combat->GetSnapshot().InstanceId == 0);
	return true;
}

// Replay acceptance: ten rounds of the full combo, each preceded by the
// component-level reset (UCombatComponent::ResetCombat + ATrainingEnemy::
// ResetEnemy - exactly the two entries the M1-027 ResetTrainingSession calls
// per participant), never leak state: every round starts fresh, produces the
// identical event and HP trajectory, never spawns or removes an actor, and
// mints strictly increasing, unique attack instance ids (the session counter
// is never rewound).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_037TenRoundsNoStateLeak,
	"UEMMO.Tasks.M1_037.TenRoundsNoStateLeak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_037TenRoundsNoStateLeak::RunTest(const FString& Parameters)
{
	UWorld* World = M1_037_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_037_ScenarioScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	TArray<FName> Round0Started;
	TArray<FName> Round0Finished;
	TArray<FName> Round0Hits;
	TArray<float> Round0Damages;
	TArray<float> Round0Hp;
	TArray<uint64> Round0Sequences;
	int32 BaselineTotalActors = 0;
	int32 BaselineEnemies = 0;
	M1_037_CountActors(*World, BaselineTotalActors, BaselineEnemies);

	TSet<uint64> SeenInstanceIds;
	uint64 LastStartedInstanceId = 0;
	double SessionNow = 0.0;

	for (int32 Round = 0; Round < 10; ++Round)
	{
		if (Round > 0)
		{
			// The unified reset entries: the same two calls the M1-027
			// service's phase 1/2 run for every participant.
			Scene.Combat->ResetCombat();
			Scene.Enemy->ResetEnemy();
		}

		// Fresh round start: full HP, both combatants Free with empty
		// buffers, the enemy grounded with cleared airborne counters, and no
		// actor spawned or removed by the previous round.
		const FCombatSnapshot AttackerFresh = Scene.Combat->GetSnapshot();
		const FCombatSnapshot EnemyFresh = Scene.EnemyCombat->GetSnapshot();
		int32 TotalActors = 0;
		int32 EnemyActors = 0;
		M1_037_CountActors(*World, TotalActors, EnemyActors);
		TestTrue(FString::Printf(TEXT("round %d: the reset left the room fresh (HP 100, both Free, empty buffers, grounded enemy, cleared counters)"), Round),
			FMath::IsNearlyEqual(Scene.EnemyHealth(), 100.0f)
			&& AttackerFresh.ActionState == ECombatActionState::Free && AttackerFresh.BufferSize == 0 && AttackerFresh.InstanceId == 0
			&& EnemyFresh.ActionState == ECombatActionState::Free && EnemyFresh.BufferSize == 0 && EnemyFresh.InstanceId == 0
			&& Scene.Enemy->GetAirState() == ECombatAirState::Grounded
			&& Scene.Enemy->GetAirComboCount() == 0 && Scene.Enemy->GetLauncherCycleCount() == 0
			&& !Scene.EnemyCombat->IsInLandingRecovery());
		TestTrue(FString::Printf(TEXT("round %d: the actor set never changed (total %d, enemies %d)"), Round, TotalActors, EnemyActors),
			TotalActors == BaselineTotalActors && EnemyActors == BaselineEnemies && EnemyActors == 1);

		FM1_037_RunRecord Record;
		if (!M1_037_RunScenario(*this, Scene, Record, EM1_037_ScenarioKind::FullCombo, SessionNow))
		{
			return true;
		}
		M1_037_AssertFullComboStream(*this, Scene, Record, *FString::Printf(TEXT("round %d"), Round));
		AddInfo(FString::Printf(TEXT("Round %d event order:\n%s"), Round, *Record.EventOrder));

		if (Round == 0)
		{
			Round0Started = Record.StartedIds;
			Round0Finished = Record.FinishedIds;
			Round0Hits = Record.HitIds;
			Round0Damages = Record.HitDamages;
			Round0Hp = Record.HpAfterHit;
			Round0Sequences = Record.QueuedSequences;
		}
		else
		{
			TestTrue(FString::Printf(TEXT("round %d: the attack switch stream is identical to round 0"), Round),
				Record.StartedIds == Round0Started && Record.FinishedIds == Round0Finished);
			TestTrue(FString::Printf(TEXT("round %d: the hit and damage trajectory is identical to round 0"), Round),
				Record.HitIds == Round0Hits && Record.HitDamages == Round0Damages);
			TestTrue(FString::Printf(TEXT("round %d: the HP trajectory and the press schedule are identical to round 0"), Round),
				Record.HpAfterHit == Round0Hp && Record.QueuedSequences == Round0Sequences);
		}

		// The reset never rewinds the session counter: every started instance
		// id of this round is strictly larger than every earlier one and
		// unique across the whole session.
		for (const uint64 InstanceId : Record.StartedInstanceIds)
		{
			TestTrue(FString::Printf(TEXT("round %d: instance id %llu is strictly increasing across the session"), Round, InstanceId),
				InstanceId > LastStartedInstanceId);
			TestTrue(FString::Printf(TEXT("round %d: instance id %llu is unique across the session"), Round, InstanceId),
				!SeenInstanceIds.Contains(InstanceId));
			SeenInstanceIds.Add(InstanceId);
			LastStartedInstanceId = InstanceId;
		}
	}

	AddInfo(FString::Printf(TEXT("Ten rounds completed; 40 unique instance ids, final session clock %.4f s."), SessionNow));
	return true;
}

#endif
