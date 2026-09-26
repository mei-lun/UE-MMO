// M2-003: enemy attack telegraph and recovery. The melee brain grows the
// timed states the M2-002 chase was missing: Approach -> Telegraph (0.35 s
// wind-up from the UEnemyDefinition) -> Attack (the attack instance runs on
// the enemy's OWN combat component, the shared M1 pipeline) -> Recover
// (0.50 s rest) -> back to Approach. The wind-up locks the attack direction
// at entry and holds the position: a target that walks away during the
// wind-up is simply missed by the real 3D hit query when the attack fires.
// Nothing here ever touches a victim's health directly - damage settles
// through the combat component hit pipeline (M1-019) exactly like the
// player's own attacks.
//
// The pure attack-condition rule is unit-tested directly; the world tests
// reuse the engine FTestWorldWrapper precedent from EnemyApproachTests/
// GameCombatWiringTests: a manually ticked temp world with real actor ticks,
// real CharacterMovement walking physics and real collision (floor + four
// blocking walls). The player side is a real APrototypeCharacter with a
// UHealthComponent attached at runtime (the pawn ships none yet; the M1-019
// target query only selects actors that carry one), so the enemy's attack
// can land on it and its HP is directly observable.
#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/MeleeEnemyController.h"
#include "../PrototypeCharacter.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Templates/Function.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_003
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M2_003_FrameSeconds = 1.0f / 60.0f;

	// Fine step for the telegraph/recover boundary sampling: the acceptance
	// compares the 0.34 s no-attack sample against the 0.35 s wind-up
	// deadline, which needs a finer resolution than one 60 fps frame.
	constexpr float M2_003_FineStepSeconds = 0.01f;

	// Remote scene base so the temp world can never collide with arena
	// content (a corner of its own, isolated from the M1/M2-002 suites). X
	// horizontal, Y depth, Z height; the floor top sits at the base Z.
	const FVector M2_003_SceneBase(52000.0, 45000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_003_FloorHalfThickness = 100.0f;
	const float M2_003_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M2_003_PlayerSpawnHeight = 120.0f;
	const float M2_003_EnemySpawnHeight = 90.0f;

	// Rectangular field: interior X within +/-800, Y within +/-500 of the
	// scene base. Four blocking walls seal it; RoomBounds mirrors the planes.
	const float M2_003_RoomHalfExtentX = 800.0f;
	const float M2_003_RoomHalfExtentY = 500.0f;
	const float M2_003_WallHalfThickness = 25.0f;
	const float M2_003_WallHalfHeight = 200.0f;

	// Contract values (UEnemyDefinition M2-001 defaults; interface contract 7).
	const float M2_003_AttackRangeX = 160.0f;
	const float M2_003_AlignYTolerance = 35.0f;

	// Contract timings: the wind-up comes from the definition (0.35 s), the
	// recovery rest is the controller constant (0.50 s).
	const double M2_003_TelegraphSeconds = 0.35;
	const double M2_003_RecoverSeconds = 0.50;

	// Depth dodge distance used during the wind-up: the light_01 hit box half
	// extent is 50 cm in Y around the enemy line, so 300 cm is safely out.
	const float M2_003_DodgeOffsetY = 300.0f;

	// Frame caps: settle waits for ground contact; the state/predicate waits
	// use simulated-seconds caps that cover the whole attack cycle several
	// times over (wind-up 0.35 + instance ~0.43 + recover 0.50).
	constexpr int32 M2_003_MaxSettleFrames = 300;
	constexpr double M2_003_MaxStateWaitSeconds = 6.0;
	constexpr double M2_003_MaxInstanceWaitSeconds = 3.0;

	// The light_01 attack id every enemy of this suite fires (the M1 catalog
	// entry with base damage 10 and an X-forward hit box).
	const FName M2_003_AttackId = FName(TEXT("light_01"));

	// World-static blocking box (floor and walls share the builder). The
	// standard BlockAll profile is the collision a real level carries.
	static AActor* M2_003_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor, Name);
		Actor->SetRootComponent(Box);
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetBoxExtent(HalfExtent);
		Box->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Box->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Box->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top and enables
	// no-controller physics (the M1-041 two-step activation) so it settles
	// like a possessed game pawn. The enemy attack pipeline (M1-019) only
	// selects targets that carry a UHealthComponent and the prototype pawn
	// ships none yet, so the test attaches one at runtime: the player becomes
	// hittable through the exact same health path every other combatant uses.
	static APrototypeCharacter* M2_003_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M2_003_SceneBase + FVector(0.0, 0.0, M2_003_PlayerSpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Player == nullptr)
		{
			return nullptr;
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
		UHealthComponent* PlayerHealth = NewObject<UHealthComponent>(Player, TEXT("M2_003_PlayerHealth"));
		if (PlayerHealth != nullptr)
		{
			PlayerHealth->RegisterComponent();
		}
		return Player;
	}

	// Spawns the melee enemy at the given absolute location. The enemy falls
	// onto the floor before its controller possesses it (the constructor's
	// bRunPhysicsWithNoController covers that window); possession switches
	// the movement to controller-driven physics.
	static AMeleeEnemy* M2_003_SpawnEnemy(UWorld& World, const FVector& SpawnLocation)
	{
		FActorSpawnParameters Params;
		AMeleeEnemy* Enemy = World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(), SpawnLocation, FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// The M2-001 definition every enemy of this suite runs with: the melee
	// attack id of the M1 catalog plus the contract defaults (MaxHP 60,
	// AttackPower 0 -> light_01 lands exactly 10, MoveSpeed 220, range 160,
	// depth tolerance 35, wind-up 0.35 s). Pure runtime object: no asset.
	static UEnemyDefinition* M2_003_MakeDefinition()
	{
		UEnemyDefinition* Definition = NewObject<UEnemyDefinition>(GetTransientPackage());
		Definition->EnemyId = FName(TEXT("melee_grunt"));
		Definition->MeleeAttackId = M2_003_AttackId;
		return Definition;
	}

	// Recorded enemy-side combat events (the assertions read this only).
	struct FM2_003_Events
	{
		TArray<FName> Started;
		int32 Hits = 0;
		TArray<FName> HitIds;
	};

	// One full scene: floor + four walls + player + one possessed melee
	// enemy, all inside a fully ticked temp world.
	struct FM2_003_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UHealthComponent* PlayerHealth = nullptr;
		AMeleeEnemy* Enemy = nullptr;
		AMeleeEnemyController* Controller = nullptr;
		UCombatComponent* EnemyCombat = nullptr;
		FBox RoomBounds = FBox(ForceInit);
		FM2_003_Events Events;

		// Builds the world and its occupants. Returns false after reporting
		// the problem so the caller can bail.
		bool Build(FAutomationTestBase& Test, const FVector& EnemyOffset)
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

			AActor* Floor = M2_003_SpawnBoxActor(*World,
				M2_003_SceneBase - FVector(0.0, 0.0, M2_003_FloorHalfThickness),
				FVector(M2_003_FloorHalfExtentXY, M2_003_FloorHalfExtentXY, M2_003_FloorHalfThickness),
				TEXT("M2_003_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first probe/tick (M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			// Precondition probe: the walking physics needs the floor to stop
			// a falling pawn capsule. Reported first so a missing blocker can
			// never masquerade as a telegraph failure.
			FHitResult ProbeHit(NoInit);
			ProbeHit.Init();
			const FCollisionShape ProbeShape = FCollisionShape::MakeCapsule(36.0f, 96.0f);
			const bool bFloorBlocks = World->SweepSingleByChannel(
				ProbeHit,
				M2_003_SceneBase + FVector(0.0, 0.0, M2_003_PlayerSpawnHeight),
				M2_003_SceneBase - FVector(0.0, 0.0, 2.0 * M2_003_FloorHalfThickness),
				FQuat::Identity,
				ECC_Pawn,
				ProbeShape,
				FCollisionQueryParams(TEXT("M2_003_FloorProbe"), false));
			if (!Test.TestTrue(TEXT("the floor blocks a pawn capsule sweep"), bFloorBlocks))
			{
				return false;
			}

			// Four blocking walls sealing the rectangular field.
			const float WallShiftX = M2_003_RoomHalfExtentX + M2_003_WallHalfThickness;
			const float WallShiftY = M2_003_RoomHalfExtentY + M2_003_WallHalfThickness;
			const float WallSpanX = M2_003_RoomHalfExtentX + 2.0f * M2_003_WallHalfThickness;
			const float WallSpanY = M2_003_RoomHalfExtentY + 2.0f * M2_003_WallHalfThickness;
			M2_003_SpawnBoxActor(*World, M2_003_SceneBase + FVector(-WallShiftX, 0.0, M2_003_WallHalfHeight),
				FVector(M2_003_WallHalfThickness, WallSpanY, M2_003_WallHalfHeight), TEXT("M2_003_WallNegX"));
			M2_003_SpawnBoxActor(*World, M2_003_SceneBase + FVector(WallShiftX, 0.0, M2_003_WallHalfHeight),
				FVector(M2_003_WallHalfThickness, WallSpanY, M2_003_WallHalfHeight), TEXT("M2_003_WallPosX"));
			M2_003_SpawnBoxActor(*World, M2_003_SceneBase + FVector(0.0, -WallShiftY, M2_003_WallHalfHeight),
				FVector(WallSpanX, M2_003_WallHalfThickness, M2_003_WallHalfHeight), TEXT("M2_003_WallNegY"));
			M2_003_SpawnBoxActor(*World, M2_003_SceneBase + FVector(0.0, WallShiftY, M2_003_WallHalfHeight),
				FVector(WallSpanX, M2_003_WallHalfThickness, M2_003_WallHalfHeight), TEXT("M2_003_WallPosY"));

			// The logical room bound mirrors the wall planes (the filter only
			// reads X/Y; the Z extent only makes the box well-formed).
			RoomBounds = FBox(
				FVector(M2_003_SceneBase.X - M2_003_RoomHalfExtentX, M2_003_SceneBase.Y - M2_003_RoomHalfExtentY, 0.0),
				FVector(M2_003_SceneBase.X + M2_003_RoomHalfExtentX, M2_003_SceneBase.Y + M2_003_RoomHalfExtentY, 2.0 * M2_003_WallHalfHeight));

			Player = M2_003_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerHealth = Player->FindComponentByClass<UHealthComponent>();
			if (!Test.TestTrue(TEXT("the player carries the test-attached health pool"), PlayerHealth != nullptr))
			{
				return false;
			}

			Enemy = M2_003_SpawnEnemy(*World, M2_003_SceneBase + EnemyOffset + FVector(0.0, 0.0, M2_003_EnemySpawnHeight));
			if (!Test.TestNotNull(TEXT("the melee enemy spawns"), Enemy))
			{
				return false;
			}
			Enemy->SetEnemyDefinition(M2_003_MakeDefinition());
			EnemyCombat = Enemy->GetCombatComponent();
			if (!Test.TestTrue(TEXT("the enemy carries its own combat component"), EnemyCombat != nullptr))
			{
				return false;
			}

			Controller = World->SpawnActor<AMeleeEnemyController>(
				AMeleeEnemyController::StaticClass(),
				M2_003_SceneBase + FVector(0.0, 0.0, 2.0 * M2_003_WallHalfHeight + 100.0),
				FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the melee enemy controller spawns"), Controller))
			{
				return false;
			}
			Controller->SetRoomBounds(RoomBounds);
			Controller->Possess(Enemy);

			EnemyCombat->OnStarted.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Started.Add(AttackId);
			});
			EnemyCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				++Events.Hits;
				Events.HitIds.Add(Hit.AttackId);
			});
			return true;
		}

		/** Injects the chase target into the controller (weak reference). */
		void SetChaseTarget(AActor* Target)
		{
			if (Controller != nullptr)
			{
				Controller->SetTarget(Target);
			}
		}

		/** Current injected world clock value (the controller's time base). */
		double WorldSeconds() const
		{
			return World != nullptr ? World->GetTimeSeconds() : 0.0;
		}

		/** Ticks the world once with the requested delta. */
		bool TickSeconds(FAutomationTestBase& Test, float DeltaSeconds)
		{
			return Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(DeltaSeconds));
		}

		/** Ticks the world until player and enemy stand grounded. */
		bool Settle(FAutomationTestBase& Test)
		{
			for (int32 Frame = 0; Frame < M2_003_MaxSettleFrames; ++Frame)
			{
				if (!TickSeconds(Test, M2_003_FrameSeconds))
				{
					return false;
				}
				const UCharacterMovementComponent* PlayerMovement = Player != nullptr ? Player->GetCharacterMovement() : nullptr;
				const UCharacterMovementComponent* EnemyMovement = Enemy != nullptr ? Enemy->GetCharacterMovement() : nullptr;
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				const bool bEnemyGrounded = EnemyMovement != nullptr
					&& EnemyMovement->MovementMode == MOVE_Walking
					&& EnemyMovement->Velocity.Size() < 1.0f;
				if (bPlayerGrounded && bEnemyGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the combatants never settled onto the floor within the settle cap"));
			return false;
		}

		/** Ticks until the controller reaches the requested state. */
		bool WaitForState(FAutomationTestBase& Test, EMeleeEnemyState Wanted, double CapSeconds)
		{
			return WaitFor(Test,
				[this, Wanted]() { return Controller != nullptr && Controller->GetState() == Wanted; },
				Wanted == EMeleeEnemyState::Telegraph
					? TEXT("the controller enters the Telegraph wind-up")
					: TEXT("the controller reaches the expected state"),
				CapSeconds);
		}

		/** Ticks (default 60 fps steps) until the predicate holds. */
		bool WaitFor(FAutomationTestBase& Test, const TFunction<bool()>& Predicate, const TCHAR* What, double CapSeconds,
			float StepSeconds = M2_003_FrameSeconds)
		{
			double Elapsed = 0.0;
			while (Elapsed < CapSeconds)
			{
				if (Predicate())
				{
					return true;
				}
				if (!TickSeconds(Test, StepSeconds))
				{
					return false;
				}
				Elapsed += StepSeconds;
			}
			Test.AddError(FString::Printf(TEXT("never observed: %s (within %.2f simulated seconds)"), What, CapSeconds));
			return false;
		}

		/** Pins the player to a spot (the dodge/pin puppet the tests move). */
		void PinPlayer(const FVector& Location)
		{
			if (Player == nullptr)
			{
				return;
			}
			Player->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
			if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
			{
				Movement->Velocity = FVector::ZeroVector;
			}
		}
	};
}

using namespace UE::UEMMO::Tasks::M2_003;

// Pure rule, table driven: the attack condition equals the approach hold
// band. Boundary offsets (exactly the range, exactly the depth tolerance)
// count as ready; one cm beyond either axis is not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003AttackConditionTableBoundaryInclusive,
	"UEMMO.Tasks.M2_003.AttackConditionTableBoundaryInclusive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003AttackConditionTableBoundaryInclusive::RunTest(const FString& Parameters)
{
	struct FReadyRow
	{
		float OffsetX;
		float OffsetY;
		bool bExpected;
		const TCHAR* Description;
	};
	const FReadyRow Rows[] = {
		{ 160.0f, 0.0f, true, TEXT("offset exactly at the attack range counts as ready") },
		{ -160.0f, 0.0f, true, TEXT("negative offset exactly at the attack range counts as ready") },
		{ 161.0f, 0.0f, false, TEXT("one cm beyond the attack range is not ready") },
		{ -161.0f, 0.0f, false, TEXT("one cm beyond the attack range on the -X side is not ready") },
		{ 0.0f, 35.0f, true, TEXT("offset exactly at the depth tolerance counts as ready") },
		{ 0.0f, -35.0f, true, TEXT("negative offset exactly at the depth tolerance counts as ready") },
		{ 0.0f, 36.0f, false, TEXT("one cm beyond the depth tolerance is not ready") },
		{ 100.0f, 20.0f, true, TEXT("aligned and in range is ready (the hold band is the attack window)") },
		{ 100.0f, 40.0f, false, TEXT("depth misalignment blocks the attack even inside the range") },
		{ 200.0f, 0.0f, false, TEXT("range overshoot blocks the attack even when depth aligned") },
	};
	for (const FReadyRow& Row : Rows)
	{
		const FVector EnemyLocation(0.0f, 0.0f, 0.0f);
		const FVector TargetLocation(Row.OffsetX, Row.OffsetY, 0.0f);
		TestTrue(Row.Description,
			ComputeMeleeAttackReady(EnemyLocation, TargetLocation, M2_003_AttackRangeX, M2_003_AlignYTolerance) == Row.bExpected);
	}
	return true;
}

// Acceptance: at 0.34 s of wind-up no attack instance exists (the combat
// component snapshot is still Free, no Started event); the instance starts
// only past the 0.35 s wind-up deadline (Started event, Attacking snapshot,
// controller in the Attack state while the instance runs).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003WorldTelegraphDelaysAttackPast034s,
	"UEMMO.Tasks.M2_003.WorldTelegraphDelaysAttackPast034s",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003WorldTelegraphDelaysAttackPast034s::RunTest(const FString& Parameters)
{
	FM2_003_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxStateWaitSeconds))
	{
		return true;
	}
	const double TelegraphStart = Scene.Controller->GetStateEnteredGameSeconds();

	// Phase A: sample the wind-up in 10 ms steps and stop at the first sample
	// at or past 0.34 s. No attack instance may exist at any of the samples.
	bool bFiredEarly = false;
	while (Scene.WorldSeconds() - TelegraphStart < 0.34)
	{
		if (!Scene.TickSeconds(*this, M2_003_FineStepSeconds))
		{
			return true;
		}
		if (Scene.Events.Started.Num() > 0)
		{
			bFiredEarly = true;
			break;
		}
	}
	const double SampledElapsed = Scene.WorldSeconds() - TelegraphStart;
	TestTrue(FString::Printf(TEXT("the wind-up ran to the 0.34 s sample (elapsed %.3f s)"), SampledElapsed),
		!bFiredEarly && SampledElapsed >= 0.34 - 1e-3);
	TestEqual(TEXT("at 0.34 s no attack instance exists (no Started event)"), Scene.Events.Started.Num(), 0);
	TestTrue(TEXT("at 0.34 s the combat component snapshot is still Free"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);

	// Phase B: keep sampling until the instance starts; it must be running by
	// 0.37 s (the 0.35 s deadline plus frame tolerance) and never earlier
	// than the deadline window.
	bool bStarted = false;
	double ElapsedAtStart = -1.0;
	while (Scene.WorldSeconds() - TelegraphStart <= M2_003_TelegraphSeconds + 0.02)
	{
		if (!Scene.TickSeconds(*this, M2_003_FineStepSeconds))
		{
			return true;
		}
		if (Scene.Events.Started.Num() > 0)
		{
			bStarted = true;
			ElapsedAtStart = Scene.WorldSeconds() - TelegraphStart;
			break;
		}
	}
	TestTrue(FString::Printf(TEXT("the attack instance started after the 0.35 s wind-up (first Started at %.3f s elapsed)"),
		ElapsedAtStart), bStarted);
	if (bStarted)
	{
		TestTrue(FString::Printf(TEXT("the attack fired no earlier than the deadline window (%.3f s elapsed)"), ElapsedAtStart),
			ElapsedAtStart >= M2_003_TelegraphSeconds - 1e-3);
		TestEqual(TEXT("the wind-up fired exactly the light_01 attack"), Scene.Events.Started[0], M2_003_AttackId);
		TestTrue(TEXT("the combat component snapshot is Attacking right after the wind-up fired"),
			Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Attacking);
		TestTrue(TEXT("the controller shows the Attack state while the instance runs"),
			Scene.Controller->GetState() == EMeleeEnemyState::Attack);
	}
	return true;
}

// Acceptance: a target that moves out of the depth band during the wind-up
// is missed - the attack instance still fires (the lock happened at the
// wind-up start) but the real 3D hit query finds nothing, so no hit is
// confirmed and the player HP stays untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003WorldDodgeDuringTelegraphMisses,
	"UEMMO.Tasks.M2_003.WorldDodgeDuringTelegraphMisses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003WorldDodgeDuringTelegraphMisses::RunTest(const FString& Parameters)
{
	FM2_003_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxStateWaitSeconds))
	{
		return true;
	}
	const float HpBefore = Scene.PlayerHealth->GetHealth();

	// The dodge happens mid-wind-up: a few locked frames, then the target
	// steps 300 cm out of the depth band (the tests move the player puppet
	// directly; the enemy side is never injected with anything).
	if (!Scene.TickSeconds(*this, 6.0f * M2_003_FrameSeconds))
	{
		return true;
	}
	Scene.PinPlayer(Scene.Player->GetActorLocation() + FVector(0.0f, M2_003_DodgeOffsetY, 0.0f));

	// The wind-up completes and fires its attack instance regardless.
	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.Events.Started.Num() > 0; },
		TEXT("the wind-up fires its attack instance"), M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	TestEqual(TEXT("the wind-up fired exactly one attack despite the dodge"), Scene.Events.Started.Num(), 1);
	TestEqual(TEXT("the fired attack is light_01"), Scene.Events.Started[0], M2_003_AttackId);

	// The instance runs out without ever finding the dodged target.
	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free; },
		TEXT("the attack instance finishes"), M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	TestEqual(TEXT("the dodged attack confirmed no hit"), Scene.Events.Hits, 0);
	TestEqual(FString::Printf(TEXT("the dodging player kept its HP (before %.1f, after %.1f)"),
		HpBefore, Scene.PlayerHealth->GetHealth()), Scene.PlayerHealth->GetHealth(), HpBefore, 0.01f);
	return true;
}

// Acceptance: a stationary target in range takes the attack - the wind-up
// fires light_01 through the enemy's own combat component and the shared
// M1-019 pipeline deducts exactly the light_01 damage (10 HP with the
// definition's AttackPower 0), exactly once per instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003WorldStationaryTargetTakesLight01Hit,
	"UEMMO.Tasks.M2_003.WorldStationaryTargetTakesLight01Hit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003WorldStationaryTargetTakesLight01Hit::RunTest(const FString& Parameters)
{
	FM2_003_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxStateWaitSeconds))
	{
		return true;
	}
	const float HpBefore = Scene.PlayerHealth->GetHealth();

	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.Events.Hits > 0; },
		TEXT("the fired attack lands on the stationary target"), M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	TestEqual(TEXT("exactly one attack was started before the hit"), Scene.Events.Started.Num(), 1);
	TestEqual(TEXT("the hit landed as light_01"), Scene.Events.HitIds[0], M2_003_AttackId);
	TestEqual(FString::Printf(TEXT("the hit deducted exactly the light_01 damage (HP %.1f -> %.1f)"),
		HpBefore, Scene.PlayerHealth->GetHealth()), Scene.PlayerHealth->GetHealth(), HpBefore - 10.0f, 0.01f);

	// The instance retires; one instance lands exactly one hit.
	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free; },
		TEXT("the attack instance finishes"), M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	TestEqual(TEXT("one attack instance lands exactly one hit"), Scene.Events.Hits, 1);
	return true;
}

// Acceptance: after the attack instance finishes the enemy rests 0.50 s.
// Inside the Recover window a still-satisfied attack condition starts no new
// wind-up and fires no attack; once the rest ends the cycle restarts and the
// second attack comes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003WorldRecoverBlocksReattackUntil050s,
	"UEMMO.Tasks.M2_003.WorldRecoverBlocksReattackUntil050s",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003WorldRecoverBlocksReattackUntil050s::RunTest(const FString& Parameters)
{
	FM2_003_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxStateWaitSeconds))
	{
		return true;
	}
	// First attack cycle: wind-up, fire, hit.
	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.Events.Started.Num() > 0; },
		TEXT("the first attack starts"), M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.Events.Hits > 0; },
		TEXT("the first attack lands"), M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}

	// Pin the target to its post-hit spot: the attack condition stays
	// satisfied through the whole rest, so the Recover window is what blocks
	// the re-attack (not a lost target).
	const FVector PinnedLocation = Scene.Player->GetActorLocation();
	if (!Scene.WaitForState(*this, EMeleeEnemyState::Recover, M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	const double RecoverStart = Scene.Controller->GetStateEnteredGameSeconds();

	// Sample the rest window in 10 ms steps: state stays Recover, no new
	// wind-up, no second attack while the window runs.
	bool bSampled = false;
	while (Scene.WorldSeconds() - RecoverStart < M2_003_RecoverSeconds - 0.05)
	{
		if (!Scene.TickSeconds(*this, M2_003_FineStepSeconds))
		{
			return true;
		}
		Scene.PinPlayer(PinnedLocation);
		if (!TestTrue(TEXT("the controller stays in Recover inside the 0.50 s window"),
			Scene.Controller->GetState() == EMeleeEnemyState::Recover))
		{
			return true;
		}
		if (!TestEqual(TEXT("no re-attack inside the Recover window"), Scene.Events.Started.Num(), 1))
		{
			return true;
		}
		bSampled = true;
	}
	TestTrue(TEXT("the Recover window was sampled through its whole length"), bSampled);

	// After the rest the cycle restarts: a new wind-up (the pinned target
	// still stands in range), then the second attack.
	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxInstanceWaitSeconds))
	{
		return true;
	}
	Scene.PinPlayer(PinnedLocation);
	if (!Scene.WaitFor(*this, [this, &Scene]() { return Scene.Events.Started.Num() >= 2; },
		TEXT("the second attack starts after the recover ended"), 5.0,
		M2_003_FrameSeconds))
	{
		return true;
	}
	Scene.PinPlayer(PinnedLocation);
	TestEqual(TEXT("the second attack started only after the Recover window ended"), Scene.Events.Started.Num(), 2);
	return true;
}

// Acceptance: an accepted hit during the wind-up interrupts it through the
// M1-020 victim-side path (damage, then NotifyHitReceived -> HitStun): the
// wind-up timer is cancelled immediately (no Telegraph state, presentation
// restored) and no delayed attack ever fires past the original deadline.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003WorldHitStunInterruptCancelsTelegraph,
	"UEMMO.Tasks.M2_003.WorldHitStunInterruptCancelsTelegraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003WorldHitStunInterruptCancelsTelegraph::RunTest(const FString& Parameters)
{
	FM2_003_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxStateWaitSeconds))
	{
		return true;
	}

	// The interrupt simulates exactly the accepted-hit pair the attacker's
	// pipeline runs on the victim: ApplyDamage first, then the victim-side
	// stun request carrying the hit (zero impulse and hit stop keep the
	// scene geometry and clocks untouched; the stun duration is light_01's).
	FCombatHit Hit;
	Hit.Target = Scene.Enemy;
	Hit.Damage = 5.0f;
	Hit.StunSeconds = 0.22f;
	Hit.HitStopSeconds = 0.0f;
	Hit.Impulse = FVector::ZeroVector;
	TestTrue(TEXT("the enemy health pool accepts the interrupt damage"),
		Scene.Enemy->GetHealthComponent()->ApplyDamage(5.0f) > 0.0f);
	Scene.EnemyCombat->NotifyHitReceived(Hit);
	TestTrue(TEXT("the enemy combat component is in HitStun after the accepted hit"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	// One tick for the controller to observe the stun.
	if (!Scene.TickSeconds(*this, M2_003_FrameSeconds))
	{
		return true;
	}
	TestTrue(TEXT("the wind-up is cancelled immediately (no Telegraph state left)"),
		Scene.Controller->GetState() != EMeleeEnemyState::Telegraph);
	TestTrue(TEXT("the telegraph presentation is restored by the cancel"),
		Scene.Enemy->GetMesh()->GetRelativeScale3D().Equals(FVector::OneVector, 0.01f));

	// Watch past the original 0.35 s deadline (the interruption happened
	// mid-wind-up): the cancelled wind-up must never fire a delayed attack.
	// The 0.22 s stun also blocks any new wind-up from firing inside this
	// window (a legitimate re-attack could only come at ~0.57 s).
	const double InterruptAt = Scene.WorldSeconds();
	while (Scene.WorldSeconds() - InterruptAt < 0.36)
	{
		if (!Scene.TickSeconds(*this, M2_003_FineStepSeconds))
		{
			return true;
		}
		if (Scene.Events.Started.Num() > 0)
		{
			break;
		}
	}
	TestEqual(TEXT("the interrupted telegraph never fired a delayed attack"), Scene.Events.Started.Num(), 0);
	TestEqual(TEXT("no hit was ever confirmed from the interrupted wind-up"), Scene.Events.Hits, 0);
	return true;
}

// Acceptance: death during the wind-up cancels it immediately - the
// controller drops to Idle, the presentation is restored and no attack is
// ever fired past the original deadline.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_003WorldDeathCancelsTelegraphWithoutAttack,
	"UEMMO.Tasks.M2_003.WorldDeathCancelsTelegraphWithoutAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_003WorldDeathCancelsTelegraphWithoutAttack::RunTest(const FString& Parameters)
{
	FM2_003_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	if (!Scene.WaitForState(*this, EMeleeEnemyState::Telegraph, M2_003_MaxStateWaitSeconds))
	{
		return true;
	}

	// A lethal hit: the health pool dies and MeleeEnemy's own BeginPlay
	// wiring marks the combat component dead (death has priority).
	TestTrue(TEXT("the lethal damage empties the health pool"),
		Scene.Enemy->GetHealthComponent()->ApplyDamage(9999.0f) > 0.0f);
	if (!Scene.TickSeconds(*this, M2_003_FrameSeconds))
	{
		return true;
	}
	TestTrue(TEXT("the enemy combat component is dead after the lethal damage"), Scene.EnemyCombat->IsDead());
	TestTrue(TEXT("the death dropped the controller to Idle immediately"),
		Scene.Controller->GetState() == EMeleeEnemyState::Idle);
	TestTrue(TEXT("the telegraph presentation is restored on death"),
		Scene.Enemy->GetMesh()->GetRelativeScale3D().Equals(FVector::OneVector, 0.01f));

	// No delayed attack past the original deadline; the brain stays Idle.
	const double DiedAt = Scene.WorldSeconds();
	while (Scene.WorldSeconds() - DiedAt < 0.60)
	{
		if (!Scene.TickSeconds(*this, M2_003_FineStepSeconds))
		{
			return true;
		}
		if (Scene.Events.Started.Num() > 0)
		{
			break;
		}
	}
	TestEqual(TEXT("a dead enemy never fires the wind-up attack"), Scene.Events.Started.Num(), 0);
	TestTrue(TEXT("the controller stays Idle after death"),
		Scene.Controller->GetState() == EMeleeEnemyState::Idle);
	return true;
}

#endif
