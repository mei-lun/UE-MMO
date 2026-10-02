// M3-025: production wiring of the wave-spawned melee enemies. The M2-007
// spawner birthed AMeleeEnemy actors nobody drove: no controller was attached
// (M2-002's brain had no production caller), the Z launcher never lifted them
// (the M1-022 LaunchCharacter override was never copied) and a dead enemy kept
// standing as an untouchable corpse (no death presentation at all). The fix
// under test:
//
//   1. After a due birth the spawner attaches the M2-002 AMeleeEnemyController
//      (SpawnDefaultController) and injects the player pawn as the weak chase
//      target. The wiring is gated on the world actually having a player pawn
//      (the in-game room flow): playerless bookkeeping worlds keep the exact
//      pre-M3-025 spawn semantics their suites pin.
//   2. A spawned enemy really approaches the player on real ticks (2D
//      distance to the player shrinks monotonically until the attack band).
//   3. A vertical launch impulse (the launcher path into
//      ACharacter::LaunchCharacter) lifts the enemy (Z velocity > 0) and the
//      M1-022 AirComboCount recording counts the launch.
//   4. A killed wired enemy drops its collision immediately and is destroyed
//      after the 2 s corpse-cleanup delay (the death presentation prototype
//      standard), together with its controller; the session kill bookkeeping
//      counts exactly once.
//   5. A dead player (combat SetDead) drops the enemy back to Idle without a
//      crash (the M2-002 weak-target rule, now through the production wiring).
//
// The world scaffold is the FTestWorldWrapper precedent from
// GameCombatWiringTests/EnemyApproachTests: a manually ticked temp game world
// with real actor ticks, real CharacterMovement walking physics and real
// collision (floor + four blocking walls), the production session subsystem
// and the production session flow (SetPlayer + StartRoom + BeginWaves +
// injected session clock). The tests never attach a controller or a target
// themselves - every wired state under test comes from the production wiring.
#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/MeleeEnemyController.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_025
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_025_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M3_025_SceneBase(62000.0, 49000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_025_FloorHalfThickness = 100.0f;
	const float M3_025_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M3_025_PlayerSpawnHeight = 120.0f;
	const float M3_025_EnemySpawnHeight = 90.0f;

	// Rectangular field: interior X within +/-800, Y within +/-500 of the
	// scene base. Four blocking walls seal it.
	const float M3_025_RoomHalfExtentX = 800.0f;
	const float M3_025_RoomHalfExtentY = 500.0f;
	const float M3_025_WallHalfThickness = 25.0f;
	const float M3_025_WallHalfHeight = 200.0f;

	// Contract values (UEnemyDefinition M2-001 defaults; interface contract 7).
	const float M3_025_AttackRangeX = 160.0f;
	const float M3_025_AlignYTolerance = 35.0f;

	// Enemy birth offset relative to the player: far in depth (+Y) and far in
	// X, so the approach runs the full depth-first-then-X rule on real ticks.
	const FVector M3_025_EnemySpawnOffset(500.0, 400.0, 0.0);

	// Frame caps: settle waits for ground contact; the approach covers the
	// depth alignment plus the X close at 220 cm/s with margin.
	constexpr int32 M3_025_MaxSettleFrames = 300;
	constexpr int32 M3_025_MaxApproachFrames = 900;

	// The death presentation prototype standard: a dead wired enemy is removed
	// 2 s after its death.
	constexpr double M3_025_CorpseCleanupSeconds = 2.0;

	// Per-frame distance jitter tolerance of the monotonic-approach assertion
	// (walking physics resamples the velocity every frame; the approach itself
	// is strictly distance-decreasing, the tolerance only absorbs float noise).
	const float M3_025_DistanceJitterTolerance = 0.5f;

	// Attack-band slack of the arrival assertion (10 cm around the contract
	// geometry, the braking distance the real movement needs to settle).
	const float M3_025_BandArrivalSlack = 10.0f;

	// Launcher impulse used by the lift test (the launcher definition's 700
	// cm/s launch speed; the impulse itself is the M1-022 contract value).
	const float M3_025_LauncherImpulseZ = 700.0f;

	// World-static blocking box (floor and walls share the builder). The
	// standard BlockAll profile is the collision a real level carries.
	static AActor* M3_025_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
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
	static APrototypeCharacter* M3_025_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_025_SceneBase + FVector(0.0, 0.0, M3_025_PlayerSpawnHeight),
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

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-008/M2-014 suite precedent: the progression takes the definition as
	// a parameter). MeleeAttackId stays None on purpose: this suite tests the
	// wiring, not the M2-003 attack pipeline, so the enemy never starts a
	// wind-up and the approach state stays the stable terminal state.
	static UEnemyDefinition* M3_025_MakeEnemyDef()
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = FName(TEXT("melee_grunt"));
		Def->MaxHP = 60.0f;
		Def->AttackPower = 0.0f;
		Def->MoveSpeed = 220.0f;
		Def->AttackRangeX = M3_025_AttackRangeX;
		Def->AlignYTolerance = M3_025_AlignYTolerance;
		Def->TelegraphSeconds = 0.35f;
		Def->SpawnGraceSeconds = 0.5f;
		return Def;
	}

	// Hand-built one-enemy room definition (the M2-007/M2-014 runtime
	// definition precedent): a single wave with a single enemy born far from
	// the player, so one clock injection births the whole wave.
	static URoomDefinition* M3_025_MakeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m3_025_wiring"));
		Room->RewardTableId = FName(TEXT("reward_m3_025"));

		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 1;
		Wave0.SpawnLocations.Add(M3_025_SceneBase + M3_025_EnemySpawnOffset + FVector(0.0, 0.0, M3_025_EnemySpawnHeight));
		Room->Waves.Add(Wave0);
		return Room;
	}

	// One wired scene: floor + walls + player + player controller + the real
	// session flow, with the wave's single enemy born through the production
	// session pump (SetSessionClockSeconds) - never by a test-side spawn.
	struct FM3_025_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		APlayerController* PlayerController = nullptr;
		AMeleeEnemy* Enemy = nullptr;
		AMeleeEnemyController* EnemyController = nullptr;
		double ClockSeconds = 0.0;

		// Builds the world, the production session flow and (via the pump)
		// the wired enemy. Returns false after reporting the problem so the
		// caller can bail.
		bool Build(FAutomationTestBase& Test)
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
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}

			AActor* Floor = M3_025_SpawnBoxActor(*World,
				M3_025_SceneBase - FVector(0.0, 0.0, M3_025_FloorHalfThickness),
				FVector(M3_025_FloorHalfExtentXY, M3_025_FloorHalfExtentXY, M3_025_FloorHalfThickness),
				TEXT("M3_025_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first probe/tick (M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			// Four blocking walls sealing the rectangular field.
			const float WallShiftX = M3_025_RoomHalfExtentX + M3_025_WallHalfThickness;
			const float WallShiftY = M3_025_RoomHalfExtentY + M3_025_WallHalfThickness;
			const float WallSpanX = M3_025_RoomHalfExtentX + 2.0f * M3_025_WallHalfThickness;
			const float WallSpanY = M3_025_RoomHalfExtentY + 2.0f * M3_025_WallHalfThickness;
			M3_025_SpawnBoxActor(*World, M3_025_SceneBase + FVector(-WallShiftX, 0.0, M3_025_WallHalfHeight),
				FVector(M3_025_WallHalfThickness, WallSpanY, M3_025_WallHalfHeight), TEXT("M3_025_WallNegX"));
			M3_025_SpawnBoxActor(*World, M3_025_SceneBase + FVector(WallShiftX, 0.0, M3_025_WallHalfHeight),
				FVector(M3_025_WallHalfThickness, WallSpanY, M3_025_WallHalfHeight), TEXT("M3_025_WallPosX"));
			M3_025_SpawnBoxActor(*World, M3_025_SceneBase + FVector(0.0, -WallShiftY, M3_025_WallHalfHeight),
				FVector(WallSpanX, M3_025_WallHalfThickness, M3_025_WallHalfHeight), TEXT("M3_025_WallNegY"));
			M3_025_SpawnBoxActor(*World, M3_025_SceneBase + FVector(0.0, WallShiftY, M3_025_WallHalfHeight),
				FVector(WallSpanX, M3_025_WallHalfThickness, M3_025_WallHalfHeight), TEXT("M3_025_WallPosY"));

			Player = M3_025_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			// The production wiring resolves the chase target from the world's
			// first player controller, so the scene spawns one and possesses
			// the pawn with it - the in-game possession form, and the same
			// production read ARoomTrigger uses for its HUD resolution.
			PlayerController = World->SpawnActor<APlayerController>(
				APlayerController::StaticClass(),
				M3_025_SceneBase + FVector(0.0, 0.0, 2.0 * M3_025_WallHalfHeight + 100.0),
				FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the player controller spawns"), PlayerController))
			{
				return false;
			}
			PlayerController->Possess(Player);

			// The production session flow: register the player pawn, start the
			// run, begin the one-enemy wave progression. Births ride the
			// injected session clock (the game frame driver's duty).
			Session->SetPlayer(Player);
			URoomDefinition* Room = M3_025_MakeRoom();
			UEnemyDefinition* Def = M3_025_MakeEnemyDef();
			if (!Test.TestTrue(TEXT("the room run starts"), Session->StartRoom(Room)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("the wave progression begins"), Session->BeginWaves(Room, Def)))
			{
				return false;
			}
			return true;
		}

		/** Injects the exact clock value (births ride the session pump). */
		void InjectClock(double NowSeconds)
		{
			ClockSeconds = NowSeconds;
			Session->SetSessionClockSeconds(NowSeconds);
		}

		/**
		 * Births the wave's single enemy through the production pump and
		 * captures it plus its wired controller (the cast is the test's
		 * business; BirthEnemy itself only captures).
		 */
		bool BirthEnemy(FAutomationTestBase& Test)
		{
			InjectClock(0.0);
			if (!Test.TestEqual(TEXT("the session pump birthed exactly one enemy"),
				Session->GetSpawnedEnemyCount(), 1))
			{
				return false;
			}
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				Enemy = *It;
				break;
			}
			if (!Test.TestNotNull(TEXT("the spawned melee enemy is in the world"), Enemy))
			{
				return false;
			}
			EnemyController = Cast<AMeleeEnemyController>(Enemy->GetController());
			return true;
		}

		/**
		 * Ticks the world until player and enemy walk on the floor. The
		 * velocity is deliberately NOT part of the condition: the wired enemy
		 * already chases while settling, so only the movement modes gate the
		 * wait (the approach distance measurements start grounded).
		 */
		bool Settle(FAutomationTestBase& Test)
		{
			for (int32 Frame = 0; Frame < M3_025_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_025_FrameSeconds)))
				{
					return false;
				}
				const bool bPlayerGrounded = Player != nullptr
					&& Player->GetCharacterMovement() != nullptr
					&& Player->GetCharacterMovement()->MovementMode == MOVE_Walking;
				const bool bEnemyGrounded = Enemy != nullptr
					&& Enemy->GetCharacterMovement() != nullptr
					&& Enemy->GetCharacterMovement()->MovementMode == MOVE_Walking;
				if (bPlayerGrounded && bEnemyGrounded)
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
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_025_FrameSeconds)))
				{
					return false;
				}
			}
			return true;
		}

		/**
		 * Advances the world with real 60 fps ticks while injecting the
		 * session clock alongside (the same per-frame duty a game driver
		 * performs) - this is what makes world-timer delays (the corpse
		 * lifespan) elapse through real time.
		 */
		bool AdvanceSeconds(FAutomationTestBase& Test, double Seconds)
		{
			const double StepSeconds = static_cast<double>(M3_025_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				InjectClock(ClockSeconds + StepNow);
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_025_FrameSeconds)))
				{
					return false;
				}
				Remaining -= StepNow;
			}
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_025;

// Acceptance 1: after the production birth the enemy carries its M2-002
// controller (the cast succeeds), the injected chase target is the player
// pawn, and the brain actually runs (Approach on the next ticks).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_025SpawnedEnemyGetsControllerAndTarget,
	"UEMMO.Tasks.M3_025.SpawnedEnemyGetsControllerAndTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_025SpawnedEnemyGetsControllerAndTarget::RunTest(const FString& Parameters)
{
	FM3_025_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.BirthEnemy(*this))
	{
		return true;
	}

	if (!TestNotNull(TEXT("the spawned enemy carries its AMeleeEnemyController"), Scene.EnemyController))
	{
		return true;
	}
	TestTrue(TEXT("the wired chase target is the player pawn"),
		Scene.EnemyController->GetTargetActor() == Scene.Player);

	if (!Scene.Settle(*this))
	{
		return true;
	}
	if (!Scene.TickFrames(*this, 1))
	{
		return true;
	}
	TestTrue(TEXT("the wired enemy chases the player (state Approach)"),
		Scene.EnemyController->GetState() == EMeleeEnemyState::Approach);
	return true;
}

// Acceptance 2: the wired enemy really approaches on real ticks - the 2D
// distance to the player shrinks monotonically every frame until the enemy
// arrives in the attack band (depth-first-then-X, the M2-002 rule, now driven
// by the production wiring instead of a test-side SetTarget).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_025SpawnedEnemyApproachesPlayerMonotonically,
	"UEMMO.Tasks.M3_025.SpawnedEnemyApproachesPlayerMonotonically",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_025SpawnedEnemyApproachesPlayerMonotonically::RunTest(const FString& Parameters)
{
	FM3_025_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.BirthEnemy(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	const FVector PlayerLocation = Scene.Player->GetActorLocation();
	float PreviousDistance = FVector::Dist2D(Scene.Enemy->GetActorLocation(), PlayerLocation);
	const float InitialDistance = PreviousDistance;
	bool bMonotonic = true;
	bool bReachedAttackBand = false;
	for (int32 Frame = 0; Frame < M3_025_MaxApproachFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M3_025_FrameSeconds)))
		{
			return true;
		}
		const FVector EnemyLocation = Scene.Enemy->GetActorLocation();
		const float Distance = FVector::Dist2D(EnemyLocation, PlayerLocation);
		if (Distance > PreviousDistance + M3_025_DistanceJitterTolerance)
		{
			bMonotonic = false;
		}
		PreviousDistance = Distance;
		if (FMath::Abs(EnemyLocation.X - PlayerLocation.X) <= M3_025_AttackRangeX + M3_025_BandArrivalSlack
			&& FMath::Abs(EnemyLocation.Y - PlayerLocation.Y) <= M3_025_AlignYTolerance + M3_025_BandArrivalSlack)
		{
			bReachedAttackBand = true;
			break;
		}
	}
	const float FinalDistance = PreviousDistance;

	TestTrue(TEXT("the enemy moved toward the player every frame (distance never grew beyond frame noise)"), bMonotonic);
	TestTrue(FString::Printf(TEXT("the enemy approached the player through the real wiring (distance %.1f -> %.1f cm)"),
		InitialDistance, FinalDistance), FinalDistance < InitialDistance * 0.6f);
	TestTrue(TEXT("the enemy arrived in the player's attack band within the frame cap"), bReachedAttackBand);
	return true;
}

// Acceptance 3: a vertical launch impulse (the launcher path enters the
// victim through ACharacter::LaunchCharacter) lifts the wired enemy - the Z
// velocity is positive after the next movement update - and the M1-022
// AirComboCount recording counts the launch (the M1-022 override pattern the
// melee enemy now replicates from the training enemy).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_025LauncherImpulseLiftsSpawnedEnemy,
	"UEMMO.Tasks.M3_025.LauncherImpulseLiftsSpawnedEnemy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_025LauncherImpulseLiftsSpawnedEnemy::RunTest(const FString& Parameters)
{
	FM3_025_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.BirthEnemy(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	const int32 ComboCountBefore = Scene.Enemy->GetAirComboCount();
	// The production launch dispatch: UCombatComponent::ApplyHitImpulse calls
	// LaunchCharacter through the victim's public ACharacter interface, and
	// the virtual dispatch lands in the AMeleeEnemy override. The test uses
	// the exact same call form (the override itself stays protected, the
	// ATrainingEnemy M1-022 placement).
	ACharacter& EnemyCharacter = *Scene.Enemy;
	EnemyCharacter.LaunchCharacter(FVector(0.0f, 0.0f, M3_025_LauncherImpulseZ),
		/*bXYOverride*/ false, /*bZOverride*/ true);
	TestEqual(TEXT("the vertical launch opened the launcher combo count"),
		Scene.Enemy->GetAirComboCount(), ComboCountBefore + 1);

	if (!Scene.TickFrames(*this, 1))
	{
		return true;
	}
	const float VerticalVelocity = Scene.Enemy->GetCharacterMovement()->Velocity.Z;
	TestTrue(FString::Printf(TEXT("the launcher impulse lifted the enemy (Z velocity %.1f cm/s)"), VerticalVelocity),
		VerticalVelocity > 0.0f);
	return true;
}

// Acceptance 4: a killed wired enemy drops its collision immediately, the
// session kill bookkeeping counts exactly once, and the corpse (plus its
// controller) is destroyed after the 2 s cleanup delay through the real world
// timer - no dangling actor, no crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_025DeadEnemyDropsCollisionAndDestroysAfterTwoSeconds,
	"UEMMO.Tasks.M3_025.DeadEnemyDropsCollisionAndDestroysAfterTwoSeconds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_025DeadEnemyDropsCollisionAndDestroysAfterTwoSeconds::RunTest(const FString& Parameters)
{
	FM3_025_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.BirthEnemy(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	if (!TestTrue(TEXT("the enemy health pool accepts the lethal damage"),
		Scene.Enemy->GetHealthComponent()->ApplyDamage(9999.0f) > 0.0f))
	{
		return true;
	}
	TestTrue(TEXT("the dead enemy dropped every collision immediately"),
		!Scene.Enemy->GetActorEnableCollision());
	TestTrue(TEXT("the corpse is still a valid actor right after the death"), IsValid(Scene.Enemy));
	TestEqual(TEXT("the death reached the session kill bookkeeping exactly once"),
		Scene.Session->GetKilledCount(), 1);

	if (!Scene.AdvanceSeconds(*this, M3_025_CorpseCleanupSeconds + 0.3))
	{
		return true;
	}
	TestFalse(TEXT("the corpse was destroyed after the 2 s cleanup delay"), IsValid(Scene.Enemy));
	TestFalse(TEXT("the wired controller was destroyed together with the corpse"), IsValid(Scene.EnemyController));
	return true;
}

// Acceptance 5: a dead player (combat SetDead) drops the wired enemy back to
// Idle without a crash - the M2-002 dead-target rule, now verified through
// the production wiring instead of a test-side SetTarget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_025PlayerDeathDropsSpawnedEnemyToIdle,
	"UEMMO.Tasks.M3_025.PlayerDeathDropsSpawnedEnemyToIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_025PlayerDeathDropsSpawnedEnemyToIdle::RunTest(const FString& Parameters)
{
	FM3_025_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.BirthEnemy(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	if (!TestNotNull(TEXT("the spawned enemy carries its AMeleeEnemyController"), Scene.EnemyController))
	{
		return true;
	}
	if (!Scene.TickFrames(*this, 1))
	{
		return true;
	}
	if (!TestTrue(TEXT("the wired enemy was chasing the living player before the death"),
		Scene.EnemyController->GetState() == EMeleeEnemyState::Approach))
	{
		return true;
	}

	Scene.Player->GetCombat()->SetDead(true);
	if (!Scene.TickFrames(*this, 3))
	{
		return true;
	}

	// Reaching this line already proves no crash; the brain must now be Idle.
	TestTrue(TEXT("the dead player dropped the wired enemy back to Idle"),
		Scene.EnemyController->GetState() == EMeleeEnemyState::Idle);
	TestTrue(TEXT("the enemy itself stays alive after the player death"),
		Scene.Enemy->GetHealthComponent()->IsAlive());
	return true;
}

#endif
