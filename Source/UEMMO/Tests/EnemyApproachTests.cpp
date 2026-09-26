// M2-002: melee enemy chase with depth alignment. The pure approach rule is
// "depth first, then X": while the depth offset exceeds the AlignYTolerance
// the enemy moves only along Y (never a diagonal rush at the target), once
// depth-aligned it closes along X into the AttackRangeX band and stops there.
// The controller is a Tick-driven minimal state machine (no behavior tree)
// over a weak-referenced target, a configurable rectangular room bound and a
// minimum-separation rule between melee enemies.
//
// The pure rules are unit-tested directly; the world tests reuse the engine
// FTestWorldWrapper precedent from GameCombatWiringTests/EnemyCombatDriverTests:
// a manually ticked temp world with real actor ticks, real CharacterMovement
// walking physics and real collision (floor + four blocking walls). The
// controller drives the enemy through the production entry points only
// (Possess + SetTarget + AddMovementInput inside its own Tick); the tests
// never inject movement themselves.
#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/MeleeEnemyController.h"
#include "../PrototypeCharacter.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_002
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M2_002_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (a corner of its own, isolated from the M1 suites). X
	// horizontal, Y depth, Z height; the floor top sits at the base Z.
	const FVector M2_002_SceneBase(48000.0, 45000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_002_FloorHalfThickness = 100.0f;
	const float M2_002_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M2_002_PlayerSpawnHeight = 120.0f;
	const float M2_002_EnemySpawnHeight = 90.0f;

	// Rectangular field: interior X within +/-800, Y within +/-500 of the
	// scene base. Four blocking walls seal it; RoomBounds mirrors the planes.
	const float M2_002_RoomHalfExtentX = 800.0f;
	const float M2_002_RoomHalfExtentY = 500.0f;
	const float M2_002_WallHalfThickness = 25.0f;
	const float M2_002_WallHalfHeight = 200.0f;

	// Contract values (UEnemyDefinition M2-001 defaults; interface contract 7).
	const float M2_002_AttackRangeX = 160.0f;
	const float M2_002_AlignYTolerance = 35.0f;
	const float M2_002_MinSeparation = 80.0f;

	// Frame caps: settle waits for ground contact; approach covers the depth
	// alignment plus the X close at 220 cm/s with margin; the crowd case adds
	// the separation settling time; the tail window samples the final two
	// seconds for the stable (no longer converging) invariants.
	constexpr int32 M2_002_MaxSettleFrames = 300;
	constexpr int32 M2_002_MaxApproachFrames = 900;
	constexpr int32 M2_002_MaxCrowdFrames = 1500;
	constexpr int32 M2_002_TailFrames = 120;

	// The separation assertion carries a 10 cm tolerance around the 80 cm
	// minimum (the force pair can dip slightly under the minimum while the
	// approach and separation inputs trade places around the equilibrium).
	const float M2_002_SeparationAssertMin = M2_002_MinSeparation - 10.0f;

	// Component-wise exact comparison for the pure-rule tables.
	static bool M2_002_VectorsEqual(const FVector& Actual, const FVector& Expected, float Tolerance = 1e-4f)
	{
		return FMath::Abs(Actual.X - Expected.X) <= Tolerance
			&& FMath::Abs(Actual.Y - Expected.Y) <= Tolerance
			&& FMath::Abs(Actual.Z - Expected.Z) <= Tolerance;
	}

	// World-static blocking box (floor and walls share the builder). The
	// standard BlockAll profile is the collision a real level carries.
	static AActor* M2_002_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
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
	// like a possessed game pawn before the tests read it.
	static APrototypeCharacter* M2_002_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M2_002_SceneBase + FVector(0.0, 0.0, M2_002_PlayerSpawnHeight),
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
		return Player;
	}

	// Spawns one melee enemy at the given absolute location. The enemy falls
	// onto the floor before its controller possesses it (the constructor's
	// bRunPhysicsWithNoController covers that window); possession switches
	// the movement to controller-driven physics.
	static AMeleeEnemy* M2_002_SpawnEnemy(UWorld& World, const FVector& SpawnLocation)
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

	// One full scene: floor + four walls + player + N possessed melee
	// enemies, all inside a fully ticked temp world.
	struct FM2_002_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		FBox RoomBounds = FBox(ForceInit);
		TArray<AMeleeEnemy*> Enemies;
		TArray<AMeleeEnemyController*> Controllers;

		// Builds the world and its occupants. Returns false after reporting
		// the problem so the caller can bail.
		bool Build(FAutomationTestBase& Test, const TArray<FVector>& EnemyOffsets)
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

			AActor* Floor = M2_002_SpawnBoxActor(*World,
				M2_002_SceneBase - FVector(0.0, 0.0, M2_002_FloorHalfThickness),
				FVector(M2_002_FloorHalfExtentXY, M2_002_FloorHalfExtentXY, M2_002_FloorHalfThickness),
				TEXT("M2_002_Floor"));
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
			// never masquerade as an approach failure.
			FHitResult ProbeHit(NoInit);
			ProbeHit.Init();
			const FCollisionShape ProbeShape = FCollisionShape::MakeCapsule(36.0f, 96.0f);
			const bool bFloorBlocks = World->SweepSingleByChannel(
				ProbeHit,
				M2_002_SceneBase + FVector(0.0, 0.0, M2_002_PlayerSpawnHeight),
				M2_002_SceneBase - FVector(0.0, 0.0, 2.0 * M2_002_FloorHalfThickness),
				FQuat::Identity,
				ECC_Pawn,
				ProbeShape,
				FCollisionQueryParams(TEXT("M2_002_FloorProbe"), false));
			if (!Test.TestTrue(TEXT("the floor blocks a pawn capsule sweep"), bFloorBlocks))
			{
				return false;
			}

			// Four blocking walls sealing the rectangular field.
			const float WallShiftX = M2_002_RoomHalfExtentX + M2_002_WallHalfThickness;
			const float WallShiftY = M2_002_RoomHalfExtentY + M2_002_WallHalfThickness;
			const float WallSpanX = M2_002_RoomHalfExtentX + 2.0f * M2_002_WallHalfThickness;
			const float WallSpanY = M2_002_RoomHalfExtentY + 2.0f * M2_002_WallHalfThickness;
			M2_002_SpawnBoxActor(*World, M2_002_SceneBase + FVector(-WallShiftX, 0.0, M2_002_WallHalfHeight),
				FVector(M2_002_WallHalfThickness, WallSpanY, M2_002_WallHalfHeight), TEXT("M2_002_WallNegX"));
			M2_002_SpawnBoxActor(*World, M2_002_SceneBase + FVector(WallShiftX, 0.0, M2_002_WallHalfHeight),
				FVector(M2_002_WallHalfThickness, WallSpanY, M2_002_WallHalfHeight), TEXT("M2_002_WallPosX"));
			M2_002_SpawnBoxActor(*World, M2_002_SceneBase + FVector(0.0, -WallShiftY, M2_002_WallHalfHeight),
				FVector(WallSpanX, M2_002_WallHalfThickness, M2_002_WallHalfHeight), TEXT("M2_002_WallNegY"));
			M2_002_SpawnBoxActor(*World, M2_002_SceneBase + FVector(0.0, WallShiftY, M2_002_WallHalfHeight),
				FVector(WallSpanX, M2_002_WallHalfThickness, M2_002_WallHalfHeight), TEXT("M2_002_WallPosY"));

			// The logical room bound mirrors the wall planes (the filter only
			// reads X/Y; the Z extent only makes the box well-formed).
			RoomBounds = FBox(
				FVector(M2_002_SceneBase.X - M2_002_RoomHalfExtentX, M2_002_SceneBase.Y - M2_002_RoomHalfExtentY, 0.0),
				FVector(M2_002_SceneBase.X + M2_002_RoomHalfExtentX, M2_002_SceneBase.Y + M2_002_RoomHalfExtentY, 2.0 * M2_002_WallHalfHeight));

			Player = M2_002_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}

			for (const FVector& Offset : EnemyOffsets)
			{
				AMeleeEnemy* Enemy = M2_002_SpawnEnemy(*World, M2_002_SceneBase + Offset + FVector(0.0, 0.0, M2_002_EnemySpawnHeight));
				if (!Test.TestNotNull(TEXT("the melee enemy spawns"), Enemy))
				{
					return false;
				}
				AMeleeEnemyController* Controller = World->SpawnActor<AMeleeEnemyController>(
					AMeleeEnemyController::StaticClass(),
					M2_002_SceneBase + FVector(0.0, 0.0, 2.0 * M2_002_WallHalfHeight + 100.0),
					FRotator::ZeroRotator, FActorSpawnParameters());
				if (!Test.TestNotNull(TEXT("the melee enemy controller spawns"), Controller))
				{
					return false;
				}
				Controller->SetRoomBounds(RoomBounds);
				Controller->Possess(Enemy);
				Enemies.Add(Enemy);
				Controllers.Add(Controller);
			}
			return true;
		}

		/** Injects the chase target into every controller (weak reference). */
		void SetChaseTarget(AActor* Target)
		{
			for (AMeleeEnemyController* Controller : Controllers)
			{
				if (Controller != nullptr)
				{
					Controller->SetTarget(Target);
				}
			}
		}

		/** Ticks the world until player and every enemy stand grounded. */
		bool Settle(FAutomationTestBase& Test)
		{
			for (int32 Frame = 0; Frame < M2_002_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M2_002_FrameSeconds)))
				{
					return false;
				}
				bool bAllGrounded = Player != nullptr
					&& Player->GetCharacterMovement() != nullptr
					&& Player->GetCharacterMovement()->MovementMode == MOVE_Walking
					&& Player->GetCharacterMovement()->Velocity.Size() < 1.0f;
				for (const AMeleeEnemy* Enemy : Enemies)
				{
					const UCharacterMovementComponent* Movement = Enemy ? Enemy->GetCharacterMovement() : nullptr;
					bAllGrounded = bAllGrounded && Movement != nullptr
						&& Movement->MovementMode == MOVE_Walking
						&& Movement->Velocity.Size() < 1.0f;
				}
				if (bAllGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the combatants never settled onto the floor within the settle cap"));
			return false;
		}

		/** Ticks the world the requested number of frames. */
		bool TickFrames(FAutomationTestBase& Test, int32 Frames)
		{
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M2_002_FrameSeconds)))
				{
					return false;
				}
			}
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M2_002;

// Pure rule, table driven: the depth-first approach intent. While the depth
// offset exceeds the tolerance the intent is pure Y (no matter how large the
// X offset); once aligned the intent is pure X while outside the attack
// range; aligned and inside the range the intent is zero. Boundary offsets
// (exactly the tolerance, exactly the range) count as satisfied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002ApproachIntentTableDepthFirstThenX,
	"UEMMO.Tasks.M2_002.ApproachIntentTableDepthFirstThenX",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002ApproachIntentTableDepthFirstThenX::RunTest(const FString& Parameters)
{
	struct FIntentRow
	{
		float OffsetX;
		float OffsetY;
		float ExpectedX;
		float ExpectedY;
		const TCHAR* Description;
	};
	const FIntentRow Rows[] = {
		// Deep in Y: pure Y first, even when the X offset is huge.
		{ 500.0f, 150.0f, 0.0f, 1.0f, TEXT("target deep +Y and far +X moves pure +Y") },
		{ 500.0f, -150.0f, 0.0f, -1.0f, TEXT("target deep -Y and far +X moves pure -Y") },
		{ -300.0f, 150.0f, 0.0f, 1.0f, TEXT("target deep +Y on the -X side moves pure +Y") },
		{ -300.0f, -150.0f, 0.0f, -1.0f, TEXT("target deep -Y on the -X side moves pure -Y") },
		// X already inside the attack range but depth still off: depth wins.
		{ 100.0f, 150.0f, 0.0f, 1.0f, TEXT("target in X range but deep +Y still moves pure +Y") },
		{ 100.0f, -150.0f, 0.0f, -1.0f, TEXT("target in X range but deep -Y still moves pure -Y") },
		// Depth aligned (inside the tolerance band): close along X.
		{ 500.0f, 10.0f, 1.0f, 0.0f, TEXT("aligned target far +X moves pure +X") },
		{ -500.0f, 10.0f, -1.0f, 0.0f, TEXT("aligned target far -X moves pure -X") },
		{ 500.0f, 35.0f, 1.0f, 0.0f, TEXT("offset exactly at the Y tolerance counts as aligned") },
		{ 500.0f, -35.0f, 1.0f, 0.0f, TEXT("negative offset exactly at the Y tolerance counts as aligned") },
		{ 500.0f, 36.0f, 0.0f, 1.0f, TEXT("one cm beyond the Y tolerance goes back to pure Y") },
		// Aligned and inside the range: hold position (attack is M2-003).
		{ 160.0f, 0.0f, 0.0f, 0.0f, TEXT("offset exactly at the attack range counts as in range") },
		{ -160.0f, 0.0f, 0.0f, 0.0f, TEXT("negative offset exactly at the attack range counts as in range") },
		{ 161.0f, 0.0f, 1.0f, 0.0f, TEXT("one cm beyond the attack range closes X") },
		{ -161.0f, 0.0f, -1.0f, 0.0f, TEXT("one cm beyond the attack range on the -X side closes X") },
		{ 100.0f, 20.0f, 0.0f, 0.0f, TEXT("aligned and in range holds position") },
	};
	for (const FIntentRow& Row : Rows)
	{
		const FVector EnemyLocation(0.0f, 0.0f, 0.0f);
		const FVector TargetLocation(Row.OffsetX, Row.OffsetY, 0.0f);
		const FVector Intent = ComputeMeleeApproachIntent(EnemyLocation, TargetLocation, M2_002_AttackRangeX, M2_002_AlignYTolerance);
		TestTrue(Row.Description,
			M2_002_VectorsEqual(Intent, FVector(Row.ExpectedX, Row.ExpectedY, 0.0f)));
	}
	return true;
}

// Pure rule, table driven: the minimum-separation correction between melee
// enemies (80 cm contract minimum). Beyond the minimum: nothing; below it:
// a correction of the shortfall pointing away from each close neighbor;
// exact overlap: the deterministic +X fallback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002SeparationAdjustmentTableMin80,
	"UEMMO.Tasks.M2_002.SeparationAdjustmentTableMin80",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002SeparationAdjustmentTableMin80::RunTest(const FString& Parameters)
{
	{
		const TArray<FVector> NoOthers;
		TestTrue(TEXT("no neighbors produce no separation correction"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector::ZeroVector, NoOthers, M2_002_MinSeparation), FVector::ZeroVector));
	}
	{
		const TArray<FVector> OneAt80X = { FVector(80.0f, 0.0f, 0.0f) };
		TestTrue(TEXT("a neighbor exactly at the 80 cm minimum needs no correction"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector::ZeroVector, OneAt80X, M2_002_MinSeparation), FVector::ZeroVector));
	}
	{
		const TArray<FVector> OneAt200 = { FVector(200.0f, 0.0f, 0.0f) };
		TestTrue(TEXT("a neighbor beyond the minimum is ignored"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector(0.0f, 0.0f, 0.0f), OneAt200, M2_002_MinSeparation), FVector::ZeroVector));
	}
	{
		const TArray<FVector> OneBelowAlongY = { FVector(0.0f, 0.0f, 0.0f) };
		TestTrue(TEXT("a neighbor 40 cm below pushes the enemy 40 cm up (full shortfall)"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector(0.0f, 40.0f, 0.0f), OneBelowAlongY, M2_002_MinSeparation), FVector(0.0f, 40.0f, 0.0f)));
	}
	{
		// 3-4-5 diagonal: distance 50, shortfall 30, direction (0.6, 0.8).
		const TArray<FVector> OneDiagonal = { FVector(0.0f, 0.0f, 0.0f) };
		TestTrue(TEXT("a diagonal neighbor 50 cm away pushes along the pair axis by the 30 cm shortfall"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector(30.0f, 40.0f, 0.0f), OneDiagonal, M2_002_MinSeparation), FVector(18.0f, 24.0f, 0.0f)));
	}
	{
		// Symmetric neighbors on one axis cancel out.
		const TArray<FVector> TwoSymmetric = { FVector(0.0f, 0.0f, 0.0f), FVector(80.0f, 0.0f, 0.0f) };
		TestTrue(TEXT("symmetric neighbors 40 cm away on both sides cancel"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector(40.0f, 0.0f, 0.0f), TwoSymmetric, M2_002_MinSeparation), FVector::ZeroVector));
	}
	{
		// Two close neighbors on one side sum to one correction.
		const TArray<FVector> TwoBelow = { FVector(0.0f, 60.0f, 0.0f), FVector(0.0f, -30.0f, 0.0f) };
		TestTrue(TEXT("two close neighbors sum their corrections (20 away + 50 toward = 30 up)"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector::ZeroVector, TwoBelow, M2_002_MinSeparation), FVector(0.0f, 30.0f, 0.0f)));
	}
	{
		const TArray<FVector> ExactOverlap = { FVector(5.0f, 7.0f, 0.0f) };
		TestTrue(TEXT("an exact overlap falls back to the deterministic +X push of the full minimum"),
			M2_002_VectorsEqual(ComputeMeleeSeparationAdjustment(FVector(5.0f, 7.0f, 0.0f), ExactOverlap, M2_002_MinSeparation), FVector(M2_002_MinSeparation, 0.0f, 0.0f)));
	}
	return true;
}

// Pure rule, table driven: the room-bound filter. An unset (invalid) box
// passes intents through; a location outside the rectangle stops entirely;
// inside, a component that would cross a bound is zeroed while the other
// component survives (wall sliding).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002RoomBoundsFilterStopsOutsideAndBlocksWallCrossing,
	"UEMMO.Tasks.M2_002.RoomBoundsFilterStopsOutsideAndBlocksWallCrossing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002RoomBoundsFilterStopsOutsideAndBlocksWallCrossing::RunTest(const FString& Parameters)
{
	const FBox Room(FVector(-500.0, -300.0, 0.0), FVector(500.0, 300.0, 100.0));
	{
		const FBox NoBounds = FBox(ForceInit);
		const FVector Intent(1.0f, 0.0f, 0.0f);
		TestTrue(TEXT("an unset room bound passes the intent through unchanged"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector::ZeroVector, Intent, NoBounds), Intent));
	}
	{
		TestTrue(TEXT("a location outside the +X bound stops entirely"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector(600.0f, 0.0f, 0.0f), FVector(0.0f, -1.0f, 0.0f), Room), FVector::ZeroVector));
	}
	{
		TestTrue(TEXT("a location outside the -Y bound stops entirely"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector(0.0f, -400.0f, 0.0f), FVector(1.0f, 0.0f, 0.0f), Room), FVector::ZeroVector));
	}
	{
		TestTrue(TEXT("an intent that would cross the +X wall is zeroed"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector(499.5f, 0.0f, 0.0f), FVector(1.0f, 0.0f, 0.0f), Room), FVector::ZeroVector));
	}
	{
		TestTrue(TEXT("an intent inside the room passes through"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector(400.0f, 0.0f, 0.0f), FVector(1.0f, 0.0f, 0.0f), Room), FVector(1.0f, 0.0f, 0.0f)));
	}
	{
		TestTrue(TEXT("a diagonal intent into the +Y wall keeps X and drops Y (wall slide)"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector(100.0f, 299.5f, 0.0f), FVector(1.0f, 1.0f, 0.0f), Room), FVector(1.0f, 0.0f, 0.0f)));
	}
	{
		TestTrue(TEXT("moving inward from the wall line stays allowed"),
			M2_002_VectorsEqual(FilterMeleeIntentByRoomBounds(FVector(499.5f, 0.0f, 0.0f), FVector(-1.0f, 0.0f, 0.0f), Room), FVector(-1.0f, 0.0f, 0.0f)));
	}
	return true;
}

// Pure rule: the facing rule. Only an X intent component turns the enemy
// (yaw 0 for +X, yaw 180 for -X); pure depth or zero intents keep the
// current yaw, so the enemy never faces the camera direction or the raw
// diagonal to its target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002FacingYawOnlyReflectsXMovement,
	"UEMMO.Tasks.M2_002.FacingYawOnlyReflectsXMovement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002FacingYawOnlyReflectsXMovement::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a +X intent turns the enemy to yaw 0"),
		ComputeMeleeFacingYaw(FVector(1.0f, 0.0f, 0.0f), 180.0f), 0.0f, 1e-4f);
	TestEqual(TEXT("a -X intent turns the enemy to yaw 180"),
		ComputeMeleeFacingYaw(FVector(-1.0f, 0.0f, 0.0f), 0.0f), 180.0f, 1e-4f);
	TestEqual(TEXT("a pure +Y intent keeps the current yaw"),
		ComputeMeleeFacingYaw(FVector(0.0f, 1.0f, 0.0f), 180.0f), 180.0f, 1e-4f);
	TestEqual(TEXT("a pure -Y intent keeps the current yaw"),
		ComputeMeleeFacingYaw(FVector(0.0f, -1.0f, 0.0f), 0.0f), 0.0f, 1e-4f);
	TestEqual(TEXT("a zero intent keeps the current yaw"),
		ComputeMeleeFacingYaw(FVector::ZeroVector, 90.0f), 90.0f, 1e-4f);
	return true;
}

// Acceptance: with the target 150 cm off in depth (and far in X) the enemy
// first produces pure-Y alignment movement - the X distance is not materially
// closed before the depth band is reached - and only then closes X into the
// attack range (and stays depth aligned at the stop point).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldDepthAlignsBeforeClosingX,
	"UEMMO.Tasks.M2_002.WorldDepthAlignsBeforeClosingX",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldDepthAlignsBeforeClosingX::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-500.0f, 150.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	bool bAlignedObserved = false;
	float XDistanceAtAlignment = -1.0f;
	bool bReachedAttackPosition = false;
	const AMeleeEnemy* Enemy = Scene.Enemies[0];
	for (int32 Frame = 0; Frame < M2_002_MaxApproachFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M2_002_FrameSeconds)))
		{
			break;
		}
		const FVector PlayerLocation = Scene.Player->GetActorLocation();
		const FVector EnemyLocation = Enemy->GetActorLocation();
		const float DeltaX = FMath::Abs(EnemyLocation.X - PlayerLocation.X);
		const float DeltaY = FMath::Abs(EnemyLocation.Y - PlayerLocation.Y);
		if (!bAlignedObserved && DeltaY <= M2_002_AlignYTolerance)
		{
			bAlignedObserved = true;
			XDistanceAtAlignment = DeltaX;
		}
		if (bAlignedObserved && DeltaX <= M2_002_AttackRangeX && DeltaY <= M2_002_AlignYTolerance)
		{
			bReachedAttackPosition = true;
			break;
		}
	}

	TestTrue(TEXT("the enemy depth-aligned within the frame cap"), bAlignedObserved);
	TestTrue(FString::Printf(TEXT("the X distance was not materially closed before depth alignment (at alignment X=%.1f, start 500)"), XDistanceAtAlignment),
		XDistanceAtAlignment >= 440.0f);
	TestTrue(TEXT("after alignment the enemy closed X into the attack range and stayed depth aligned"), bReachedAttackPosition);
	return true;
}

// Acceptance: an enemy already inside the attack range and depth-aligned
// stops approaching (no approach intent, the position holds).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldInRangeAlignedStopsApproach,
	"UEMMO.Tasks.M2_002.WorldInRangeAlignedStopsApproach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldInRangeAlignedStopsApproach::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-100.0f, 10.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	const FVector StartLocation = Scene.Enemies[0]->GetActorLocation();
	if (!Scene.TickFrames(*this, 180))
	{
		return true;
	}
	const FVector EndLocation = Scene.Enemies[0]->GetActorLocation();
	const float Travel = FVector::Dist2D(StartLocation, EndLocation);

	TestTrue(FString::Printf(TEXT("the in-range aligned enemy holds position over three seconds (travel=%.2f cm)"), Travel),
		Travel < 5.0f);
	const FVector PlayerLocation = Scene.Player->GetActorLocation();
	TestTrue(TEXT("the enemy is still inside the attack range and the depth band"),
		FMath::Abs(EndLocation.X - PlayerLocation.X) <= M2_002_AttackRangeX
		&& FMath::Abs(EndLocation.Y - PlayerLocation.Y) <= M2_002_AlignYTolerance);
	return true;
}

// Acceptance: destroying the target drops the controller back to Idle
// without ever dereferencing the stale weak reference (no crash).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldDestroyedTargetReturnsToIdle,
	"UEMMO.Tasks.M2_002.WorldDestroyedTargetReturnsToIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldDestroyedTargetReturnsToIdle::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-400.0f, 0.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);
	if (!Scene.TickFrames(*this, 120))
	{
		return true;
	}
	TestTrue(TEXT("the enemy was approaching before the target was destroyed"),
		Scene.Controllers[0]->GetState() == EMeleeEnemyState::Approach);

	Scene.Player->Destroy();
	if (!Scene.TickFrames(*this, 60))
	{
		return true;
	}

	// Reaching this line already proves no stale dereference crashed; the
	// state must now be Idle.
	TestTrue(TEXT("after the target was destroyed the controller returned to Idle"),
		Scene.Controllers[0]->GetState() == EMeleeEnemyState::Idle);
	return true;
}

// Acceptance: a dead target (combat component SetDead) stops the chase and
// drops the controller to Idle; the enemy brakes to a standstill instead of
// walking to the last known position.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldDeadTargetStopsChase,
	"UEMMO.Tasks.M2_002.WorldDeadTargetStopsChase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldDeadTargetStopsChase::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-400.0f, 0.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);
	if (!Scene.TickFrames(*this, 120))
	{
		return true;
	}
	TestTrue(TEXT("the enemy was approaching before the target died"),
		Scene.Controllers[0]->GetState() == EMeleeEnemyState::Approach);

	const FVector LocationAtDeath = Scene.Enemies[0]->GetActorLocation();
	Scene.Player->GetCombat()->SetDead(true);
	if (!Scene.TickFrames(*this, 60))
	{
		return true;
	}

	TestTrue(TEXT("after the target died the controller returned to Idle"),
		Scene.Controllers[0]->GetState() == EMeleeEnemyState::Idle);
	const float TravelAfterDeath = FVector::Dist2D(LocationAtDeath, Scene.Enemies[0]->GetActorLocation());
	TestTrue(FString::Printf(TEXT("the enemy braked instead of walking on to the last known position (travel=%.2f cm)"), TravelAfterDeath),
		TravelAfterDeath < 30.0f);
	return true;
}

// Acceptance: three enemies chasing the same player never stack on one
// point (pairwise distance stays at the 80 cm minimum with a 10 cm
// tolerance) and never leave the field (logical bounds and physical walls).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldThreeEnemiesChaseWithoutOverlapOrWallClip,
	"UEMMO.Tasks.M2_002.WorldThreeEnemiesChaseWithoutOverlapOrWallClip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldThreeEnemiesChaseWithoutOverlapOrWallClip::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-600.0f, 250.0f, 0.0f));
	Offsets.Add(FVector(-600.0f, 0.0f, 0.0f));
	Offsets.Add(FVector(-600.0f, -250.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	float MinTailPairDistance = TNumericLimits<float>::Max();
	float MaxTailXDistance = 0.0f;
	bool bTailAllInsideBounds = true;
	for (int32 Frame = 0; Frame < M2_002_MaxCrowdFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M2_002_FrameSeconds)))
		{
			return true;
		}
		if (Frame < M2_002_MaxCrowdFrames - M2_002_TailFrames)
		{
			continue;
		}
		// Tail window: sample the stable invariants every frame.
		for (int32 I = 0; I < Scene.Enemies.Num(); ++I)
		{
			const FVector LocationI = Scene.Enemies[I]->GetActorLocation();
			bTailAllInsideBounds = bTailAllInsideBounds && Scene.RoomBounds.IsInsideXY(LocationI);
			MaxTailXDistance = FMath::Max(MaxTailXDistance, FMath::Abs(LocationI.X - Scene.Player->GetActorLocation().X));
			for (int32 J = I + 1; J < Scene.Enemies.Num(); ++J)
			{
				const FVector LocationJ = Scene.Enemies[J]->GetActorLocation();
				MinTailPairDistance = FMath::Min(MinTailPairDistance, FVector::Dist2D(LocationI, LocationJ));
			}
		}
	}

	TestTrue(FString::Printf(TEXT("the three enemies kept at least the 80 cm minimum spacing with 10 cm tolerance (tail min=%.1f)"), MinTailPairDistance),
		MinTailPairDistance >= M2_002_SeparationAssertMin);
	TestTrue(TEXT("every enemy stayed inside the logical room bounds through the tail window"), bTailAllInsideBounds);
	TestTrue(FString::Printf(TEXT("every enemy closed to the player's attack band (tail max X distance=%.1f)"), MaxTailXDistance),
		MaxTailXDistance <= 260.0f);
	return true;
}

// Acceptance: while approaching, the facing only reflects the +/-X movement:
// it never leaves the X axis (never faces the camera direction or the raw
// diagonal), the depth-alignment phase keeps the spawn facing, and the X
// close phase faces along the actual X movement sign.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldFacingStaysOnXAxisWhileApproaching,
	"UEMMO.Tasks.M2_002.WorldFacingStaysOnXAxisWhileApproaching",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldFacingStaysOnXAxisWhileApproaching::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-500.0f, 150.0f, 0.0f));
	Offsets.Add(FVector(500.0f, -150.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	bool bAxisPureEveryFrame = true;
	bool bPreAlignFacingPreserved = true;
	bool bAligned[2] = { false, false };
	bool bSignedClosingObserved[2] = { false, false };
	const bool bExpectPositiveX[2] = { true, false };
	for (int32 Frame = 0; Frame < M2_002_MaxApproachFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M2_002_FrameSeconds)))
		{
			break;
		}
		for (int32 Index = 0; Index < Scene.Enemies.Num(); ++Index)
		{
			const AMeleeEnemy* Enemy = Scene.Enemies[Index];
			const FVector Forward = Enemy->GetActorForwardVector();
			if (FMath::Abs(Forward.Y) > 0.01f || FMath::Abs(Forward.Z) > 0.01f)
			{
				bAxisPureEveryFrame = false;
			}
			const FVector PlayerLocation = Scene.Player->GetActorLocation();
			const FVector EnemyLocation = Enemy->GetActorLocation();
			const float DeltaX = FMath::Abs(EnemyLocation.X - PlayerLocation.X);
			const float DeltaY = FMath::Abs(EnemyLocation.Y - PlayerLocation.Y);
			if (!bAligned[Index] && DeltaY > M2_002_AlignYTolerance)
			{
				// Depth phase: the spawn facing (yaw 0 = +X) must survive.
				if (Forward.X < 0.9f)
				{
					bPreAlignFacingPreserved = false;
				}
			}
			if (!bAligned[Index] && DeltaY <= M2_002_AlignYTolerance)
			{
				bAligned[Index] = true;
			}
			if (bAligned[Index] && DeltaX > M2_002_AttackRangeX)
			{
				// X close phase: face along the actual movement sign.
				if (bExpectPositiveX[Index] && Forward.X > 0.5f)
				{
					bSignedClosingObserved[Index] = true;
				}
				if (!bExpectPositiveX[Index] && Forward.X < -0.5f)
				{
					bSignedClosingObserved[Index] = true;
				}
			}
		}
		if (bAligned[0] && bAligned[1] && bSignedClosingObserved[0] && bSignedClosingObserved[1])
		{
			break;
		}
	}

	TestTrue(TEXT("the facing stayed on the X axis every frame (never toward the camera or the diagonal)"), bAxisPureEveryFrame);
	TestTrue(TEXT("the depth-alignment phase preserved the spawn facing (no Y or camera turn)"), bPreAlignFacingPreserved);
	TestTrue(TEXT("the enemy on the -X side faced +X while closing"), bSignedClosingObserved[0]);
	TestTrue(TEXT("the enemy on the +X side faced -X while closing"), bSignedClosingObserved[1]);
	return true;
}

// Acceptance: an enemy placed beyond the configurable field boundary stops
// (no approach movement) instead of steering back across the boundary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_002WorldOutOfBoundsEnemyDoesNotChase,
	"UEMMO.Tasks.M2_002.WorldOutOfBoundsEnemyDoesNotChase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_002WorldOutOfBoundsEnemyDoesNotChase::RunTest(const FString& Parameters)
{
	FM2_002_Scene Scene;
	TArray<FVector> Offsets;
	Offsets.Add(FVector(-1100.0f, 0.0f, 0.0f));
	if (!Scene.Build(*this, Offsets))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	const FVector StartLocation = Scene.Enemies[0]->GetActorLocation();
	if (!Scene.TickFrames(*this, 120))
	{
		return true;
	}
	const float Travel = FVector::Dist2D(StartLocation, Scene.Enemies[0]->GetActorLocation());
	TestTrue(FString::Printf(TEXT("the out-of-bounds enemy stopped (travel=%.2f cm over two seconds)"), Travel),
		Travel < 5.0f);
	return true;
}

#endif
