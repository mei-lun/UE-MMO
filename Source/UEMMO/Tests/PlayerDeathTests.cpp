// M2-004: player hit, death and control release. The prototype pawn becomes a
// full combatant: it ships its own UHealthComponent (MaxHP 100), so the
// M2-003 enemy attack pipeline selects it through the shared M1-018 target
// query and lands real damage on it; a lethal hit marks the combat component
// dead (attacks and movement refused, the running attack cancelled), buys the
// enemy controller's target-death exit back to Idle, and broadcasts
// PlayerDied exactly once per death lifecycle on the pawn. The F2 reset entry
// (the M0 local fallback of the M1-027 unified reset) doubles as the retry
// entry: it revives the pawn (full HP, alive, controls restored, locomotion
// animation re-attached) without duplicating any input mapping, and opens a
// fresh death lifecycle (the pawn can die again).
//
// The world tests reuse the engine FTestWorldWrapper precedent from
// EnemyTelegraphTests/EnemyApproachTests: a manually ticked temp world with
// real actor ticks, real CharacterMovement walking physics and real collision
// (floor + four blocking walls). No runtime health attachment anywhere in
// this suite: the pawn itself must carry the pool.
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
#include "EnhancedInputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "InputMappingContext.h"
#include "Templates/Function.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_004
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M2_004_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M2_004_SceneBase(64000.0, 47000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_004_FloorHalfThickness = 100.0f;
	const float M2_004_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M2_004_PlayerSpawnHeight = 120.0f;
	const float M2_004_EnemySpawnHeight = 90.0f;

	// Rectangular field: interior X within +/-800, Y within +/-500 of the
	// scene base. Four blocking walls seal it (the M2-003 proven rig).
	const float M2_004_RoomHalfExtentX = 800.0f;
	const float M2_004_RoomHalfExtentY = 500.0f;
	const float M2_004_WallHalfThickness = 25.0f;
	const float M2_004_WallHalfHeight = 200.0f;

	// Contract attack data rides on the UEnemyDefinition defaults (M2-001:
	// AttackRangeX 160, AlignYTolerance 35, Telegraph 0.35 s); the definition
	// helper below only pins the identity and the melee attack id.

	// Frame caps: settle waits for ground contact; the state/predicate waits
	// use simulated-seconds caps that cover the whole attack cycle several
	// times over (wind-up 0.35 + instance ~0.43 + recover 0.50).
	constexpr int32 M2_004_MaxSettleFrames = 300;
	constexpr double M2_004_MaxStateWaitSeconds = 6.0;
	constexpr double M2_004_MaxInstanceWaitSeconds = 3.0;

	// The light_01 attack id every enemy of this suite fires (the M1 catalog
	// entry with base damage 10 and an X-forward hit box).
	const FName M2_004_AttackId = FName(TEXT("light_01"));

	// The M1-040 DNF layout maps exactly 18 keys onto the runtime context
	// (arrows 4, X, Z, C, Space, F2, F1 and the eight skill slots); the
	// mapping-duplication guard compares against this fixed total.
	constexpr int32 M2_004_ExpectedMappingCount = 18;

	// World-static blocking box (floor and walls share the builder).
	static AActor* M2_004_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
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
	// no-controller physics so it settles like a possessed game pawn. The pawn
	// must carry its OWN UHealthComponent from spawn (M2-004): no runtime
	// attachment here, the health assertions read the shipped pool directly.
	static APrototypeCharacter* M2_004_SpawnPlayer(UWorld& World, const FVector& Offset = FVector::ZeroVector)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M2_004_SceneBase + Offset + FVector(0.0, 0.0, M2_004_PlayerSpawnHeight),
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

	// Spawns the melee enemy at the given absolute offset from the scene base.
	static AMeleeEnemy* M2_004_SpawnEnemy(UWorld& World, const FVector& Offset)
	{
		FActorSpawnParameters Params;
		AMeleeEnemy* Enemy = World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(),
			M2_004_SceneBase + Offset + FVector(0.0, 0.0, M2_004_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
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
	// attack id of the M1 catalog plus the contract defaults. Pure runtime
	// object: no asset.
	static UEnemyDefinition* M2_004_MakeDefinition()
	{
		UEnemyDefinition* Definition = NewObject<UEnemyDefinition>(GetTransientPackage());
		Definition->EnemyId = FName(TEXT("melee_grunt"));
		Definition->MeleeAttackId = M2_004_AttackId;
		return Definition;
	}

	// One full scene: floor + four walls + the shipped-health player + one
	// possessed melee enemy, all inside a fully ticked temp world.
	struct FM2_004_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UHealthComponent* PlayerHealth = nullptr;
		AMeleeEnemy* Enemy = nullptr;
		AMeleeEnemyController* Controller = nullptr;
		UCombatComponent* EnemyCombat = nullptr;
		FBox RoomBounds = FBox(ForceInit);

		// Recorded events (the assertions read these only).
		int32 PlayerDiedCount = 0;
		int32 PlayerStartedCount = 0;
		int32 PlayerFinishedCount = 0;
		int32 EnemyHits = 0;

		// Builds the world and its occupants. Returns false after reporting
		// the problem so the caller can bail. bWithPlayer=false builds the
		// pure enemy scene (the same-team test: its red/green evidence must
		// come from the friendly-fire filter alone, never from the player).
		bool Build(FAutomationTestBase& Test, const FVector& EnemyOffset, bool bWithPlayer = true)
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

			AActor* Floor = M2_004_SpawnBoxActor(*World,
				M2_004_SceneBase - FVector(0.0, 0.0, M2_004_FloorHalfThickness),
				FVector(M2_004_FloorHalfExtentXY, M2_004_FloorHalfExtentXY, M2_004_FloorHalfThickness),
				TEXT("M2_004_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first probe/tick (M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			// Four blocking walls sealing the rectangular field.
			const float WallShiftX = M2_004_RoomHalfExtentX + M2_004_WallHalfThickness;
			const float WallShiftY = M2_004_RoomHalfExtentY + M2_004_WallHalfThickness;
			const float WallSpanX = M2_004_RoomHalfExtentX + 2.0f * M2_004_WallHalfThickness;
			const float WallSpanY = M2_004_RoomHalfExtentY + 2.0f * M2_004_WallHalfThickness;
			M2_004_SpawnBoxActor(*World, M2_004_SceneBase + FVector(-WallShiftX, 0.0, M2_004_WallHalfHeight),
				FVector(M2_004_WallHalfThickness, WallSpanY, M2_004_WallHalfHeight), TEXT("M2_004_WallNegX"));
			M2_004_SpawnBoxActor(*World, M2_004_SceneBase + FVector(WallShiftX, 0.0, M2_004_WallHalfHeight),
				FVector(M2_004_WallHalfThickness, WallSpanY, M2_004_WallHalfHeight), TEXT("M2_004_WallPosX"));
			M2_004_SpawnBoxActor(*World, M2_004_SceneBase + FVector(0.0, -WallShiftY, M2_004_WallHalfHeight),
				FVector(WallSpanX, M2_004_WallHalfThickness, M2_004_WallHalfHeight), TEXT("M2_004_WallNegY"));
			M2_004_SpawnBoxActor(*World, M2_004_SceneBase + FVector(0.0, WallShiftY, M2_004_WallHalfHeight),
				FVector(WallSpanX, M2_004_WallHalfThickness, M2_004_WallHalfHeight), TEXT("M2_004_WallPosY"));

			// The player carries the health pool from spawn (no runtime
			// attachment anywhere in this suite).
			if (bWithPlayer)
			{
				Player = M2_004_SpawnPlayer(*World);
				if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
				{
					return false;
				}
				PlayerHealth = Player->FindComponentByClass<UHealthComponent>();
				if (!Test.TestTrue(TEXT("the player ships its own health pool from spawn (MaxHP 100)"),
					PlayerHealth != nullptr && PlayerHealth->GetMaxHealth() == 100.0f && PlayerHealth->IsAlive()))
				{
					return false;
				}
				Player->PlayerDied.AddLambda([this]()
				{
					++PlayerDiedCount;
				});
				Player->GetCombat()->OnStarted.AddLambda([this](FName /*AttackId*/, uint64 /*InstanceId*/)
				{
					++PlayerStartedCount;
				});
				Player->GetCombat()->OnFinished.AddLambda([this](FName /*AttackId*/, uint64 /*InstanceId*/)
				{
					++PlayerFinishedCount;
				});
			}

			Enemy = M2_004_SpawnEnemy(*World, EnemyOffset);
			if (!Test.TestNotNull(TEXT("the melee enemy spawns"), Enemy))
			{
				return false;
			}
			Enemy->SetEnemyDefinition(M2_004_MakeDefinition());
			EnemyCombat = Enemy->GetCombatComponent();
			if (!Test.TestTrue(TEXT("the enemy carries its own combat component"), EnemyCombat != nullptr))
			{
				return false;
			}
			EnemyCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& /*Hit*/)
			{
				++EnemyHits;
			});

			Controller = World->SpawnActor<AMeleeEnemyController>(
				AMeleeEnemyController::StaticClass(),
				M2_004_SceneBase + FVector(0.0, 0.0, 2.0 * M2_004_WallHalfHeight + 100.0),
				FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the melee enemy controller spawns"), Controller))
			{
				return false;
			}
			Controller->SetRoomBounds(RoomBounds);
			Controller->Possess(Enemy);
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
			for (int32 Frame = 0; Frame < M2_004_MaxSettleFrames; ++Frame)
			{
				if (!TickSeconds(Test, M2_004_FrameSeconds))
				{
					return false;
				}
				const UCharacterMovementComponent* PlayerMovement = Player != nullptr ? Player->GetCharacterMovement() : nullptr;
				const UCharacterMovementComponent* EnemyMovement = Enemy != nullptr ? Enemy->GetCharacterMovement() : nullptr;
				const bool bPlayerGrounded = Player == nullptr
					|| (PlayerMovement != nullptr
						&& PlayerMovement->MovementMode == MOVE_Walking
						&& PlayerMovement->Velocity.Size() < 1.0f);
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

		/** Ticks (default 60 fps steps) until the predicate holds. */
		bool WaitFor(FAutomationTestBase& Test, const TFunction<bool()>& Predicate, const TCHAR* What, double CapSeconds,
			float StepSeconds = M2_004_FrameSeconds)
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

		/** Pins the player to a spot (the test moves the puppet directly). */
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

using namespace UE::UEMMO::Tasks::M2_004;

// Acceptance: the enemy attack pipeline (M2-003 telegraph -> light_01 -> the
// shared M1-019 hit application) selects the player through the shared
// M1-018 target query now that the pawn ships its own health pool, and one
// landed hit deducts exactly the light_01 damage (100 -> 90).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_004WorldEnemyAttackDamagesPlayer,
	"UEMMO.Tasks.M2_004.WorldEnemyAttackDamagesPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_004WorldEnemyAttackDamagesPlayer::RunTest(const FString& Parameters)
{
	FM2_004_Scene Scene;
	if (!Scene.Build(*this, FVector(-100.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	const float HpBefore = Scene.PlayerHealth->GetHealth();
	if (!Scene.WaitFor(*this, [&Scene]() { return Scene.EnemyHits > 0; },
		TEXT("the enemy attack lands on the player"), M2_004_MaxInstanceWaitSeconds))
	{
		return true;
	}
	TestEqual(FString::Printf(TEXT("the landed enemy hit deducted exactly the light_01 damage (HP %.1f -> %.1f)"),
		HpBefore, Scene.PlayerHealth->GetHealth()), Scene.PlayerHealth->GetHealth(), HpBefore - 10.0f, 0.01f);
	TestEqual(TEXT("the player survived the first hit (no death broadcast yet)"), Scene.PlayerDiedCount, 0);
	return true;
}

// Acceptance: a lethal hit broadcasts PlayerDied exactly once, marks the
// combat component dead (death priority, snapshot stays Free), stops the
// running attack without a Finished event, and refuses every further input:
// J/K submissions start nothing, TryStartAttack refuses, the movement gate
// reads false and the pinned body never moves while dead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_004WorldLethalHitBroadcastsPlayerDiedOnceAndBlocksInput,
	"UEMMO.Tasks.M2_004.WorldLethalHitBroadcastsPlayerDiedOnceAndBlocksInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_004WorldLethalHitBroadcastsPlayerDiedOnceAndBlocksInput::RunTest(const FString& Parameters)
{
	FM2_004_Scene Scene;
	if (!Scene.Build(*this, FVector(-400.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// The player first runs a real attack instance: the death must stop it.
	if (!TestTrue(TEXT("the player starts light_01 before dying"),
		Scene.Player->GetCombat()->TryStartAttack(M2_004_AttackId, 1)))
	{
		return true;
	}
	TestTrue(TEXT("the attack instance is running before the lethal hit"),
		Scene.Player->GetCombat()->GetSnapshot().ActionState == ECombatActionState::Attacking);

	// A lethal hit through the real health pool (the M2-003 death-test
	// precedent): the pool dies, fires OnDied once, and the pawn forwards it.
	TestTrue(TEXT("the health pool accepts the lethal damage"),
		Scene.PlayerHealth->ApplyDamage(9999.0f) > 0.0f);
	TestTrue(TEXT("the health pool is dead after the lethal damage"), !Scene.PlayerHealth->IsAlive());
	TestEqual(TEXT("PlayerDied broadcast exactly once for the lethal hit"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the combat component is dead after the lethal hit"), Scene.Player->GetCombat()->IsDead());

	// Death stops the current attack: the snapshot is Free again (death is
	// tracked separately from the action state) and no Finished event ran
	// (a death cancel is an interruption, not a timeline end).
	TestTrue(TEXT("the snapshot is Free while dead (death has priority over the action state)"),
		Scene.Player->GetCombat()->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the running attack was cancelled without a Finished event"), Scene.PlayerFinishedCount, 0);

	// Dead input refusal: J/K submissions are buffered but never start
	// anything (the buffer drains expired, no Started event), and a direct
	// TryStartAttack is refused with the snapshot still Free.
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		if (!Scene.TickSeconds(*this, M2_004_FrameSeconds))
		{
			return true;
		}
	}
	Scene.Player->SubmitCombatInput(ECombatInput::Light);
	Scene.Player->SubmitCombatInput(ECombatInput::Launcher);
	Scene.Player->SubmitCombatInput(ECombatInput::Jump);
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		if (!Scene.TickSeconds(*this, M2_004_FrameSeconds))
		{
			return true;
		}
	}
	TestEqual(TEXT("dead J/K submissions start no new attack (the count is still the one pre-death instance)"),
		Scene.PlayerStartedCount, 1);
	TestFalse(TEXT("a dead component refuses a direct TryStartAttack"),
		Scene.Player->GetCombat()->TryStartAttack(M2_004_AttackId, 1));
	TestTrue(TEXT("the snapshot stays Free while dead input is refused"),
		Scene.Player->GetCombat()->GetSnapshot().ActionState == ECombatActionState::Free);

	// Movement is refused while dead: the gate reads false and the settled
	// body never moves across the dead ticks.
	TestFalse(TEXT("the movement gate refuses movement while dead"),
		Scene.Player->GetCombat()->CanAcceptMovement());
	const FVector DeadLocation = Scene.Player->GetActorLocation();
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		if (!Scene.TickSeconds(*this, M2_004_FrameSeconds))
		{
			return true;
		}
	}
	TestTrue(TEXT("the dead body never moved across the dead ticks"),
		Scene.Player->GetActorLocation().Equals(DeadLocation, 0.5f));

	// Extra damage on a dead pool is refused with no second broadcast.
	Scene.PlayerHealth->ApplyDamage(50.0f);
	TestEqual(TEXT("a dead pool never broadcasts PlayerDied twice"), Scene.PlayerDiedCount, 1);
	return true;
}

// Acceptance: after the player dies the melee controller drops the dead
// target and returns to Idle immediately (the M2-002 target-death exit), and
// it never leaves Idle again while the target stays dead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_004WorldPlayerDeathStopsEnemyChase,
	"UEMMO.Tasks.M2_004.WorldPlayerDeathStopsEnemyChase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_004WorldPlayerDeathStopsEnemyChase::RunTest(const FString& Parameters)
{
	FM2_004_Scene Scene;
	if (!Scene.Build(*this, FVector(-500.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	Scene.SetChaseTarget(Scene.Player);

	// The enemy must first be chasing: the player waits far outside the
	// attack range, so the controller walks (Approach) before the death.
	if (!Scene.WaitFor(*this, [&Scene]()
	{
		return Scene.Controller->GetState() == EMeleeEnemyState::Approach
			&& Scene.Controller->GetLastMoveIntent().Size2D() > 0.0f;
	}, TEXT("the controller approaches the living target"), M2_004_MaxStateWaitSeconds))
	{
		return true;
	}

	// The lethal hit on the health pool kills the pawn; the controller reads
	// the dead combat component and drops to Idle within a tick or two.
	TestTrue(TEXT("the health pool accepts the lethal damage"),
		Scene.PlayerHealth->ApplyDamage(9999.0f) > 0.0f);
	if (!Scene.WaitFor(*this, [&Scene]()
	{
		return Scene.Controller->GetState() == EMeleeEnemyState::Idle;
	}, TEXT("the controller drops to Idle after the target died"), 1.0))
	{
		return true;
	}
	// The dead target never re-becomes chasable.
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		if (!Scene.TickSeconds(*this, M2_004_FrameSeconds))
		{
			return true;
		}
	}
	TestTrue(TEXT("the controller stays Idle while the target is dead"),
		Scene.Controller->GetState() == EMeleeEnemyState::Idle);
	return true;
}

// Acceptance: the F2 reset entry doubles as the retry entry while dead. One
// call revives the pawn: full HP (a fresh death lifecycle), alive combat
// (dead flag dropped, buffer cleared), the body back at the spawn point, and
// the locomotion animation re-attached. A re-run of the input setup neither
// duplicates the runtime mapping nor rebuilds the actions (18 mappings stay
// 18), the restored pawn can act again, and the second lethal hit broadcasts
// PlayerDied a second time (the new lifecycle semantics match the health
// pool's ResetHealth contract).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_004WorldResetRevivesPlayerAndRestartsLifecycle,
	"UEMMO.Tasks.M2_004.WorldResetRevivesPlayerAndRestartsLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_004WorldResetRevivesPlayerAndRestartsLifecycle::RunTest(const FString& Parameters)
{
	FM2_004_Scene Scene;
	if (!Scene.Build(*this, FVector(-400.0f, 0.0f, 0.0f)))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// First lifecycle: die once.
	TestTrue(TEXT("the health pool accepts the lethal damage"),
		Scene.PlayerHealth->ApplyDamage(9999.0f) > 0.0f);
	TestEqual(TEXT("PlayerDied broadcast once for the first death"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the combat component is dead before the reset"), Scene.Player->GetCombat()->IsDead());

	// The retry entry (the F2 binding target) is callable while dead: without
	// a registered service the M0 local reset runs, which now also revives.
	Scene.Player->ResetPosition();
	TestTrue(TEXT("the reset revived the health pool (full HP, alive)"),
		Scene.PlayerHealth->IsAlive() && Scene.PlayerHealth->GetHealth() == Scene.PlayerHealth->GetMaxHealth());
	TestFalse(TEXT("the reset dropped the combat dead flag"), Scene.Player->GetCombat()->IsDead());
	TestEqual(TEXT("the reset did not add a PlayerDied broadcast"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the reset teleported the body back to the spawn point"),
		Scene.Player->GetActorLocation().Equals(M2_004_SceneBase + FVector(0.0, 0.0, M2_004_PlayerSpawnHeight), 0.5f));

	// Mapping duplication guard: the retry flow may re-possess the pawn, so
	// the input setup runs twice against the mapping context; the DNF layout
	// stays exactly its 18 mappings (no duplicated context entries).
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();
	Scene.Player->SetupPlayerInputComponent(Input);
	const UInputMappingContext* Mapping = Scene.Player->GetRuntimeInputMappingContext();
	if (!TestNotNull(TEXT("the runtime mapping context exists after input setup"), Mapping))
	{
		return true;
	}
	TestEqual(TEXT("the DNF layout maps exactly 18 keys after the first setup"),
		Mapping->GetMappings().Num(), M2_004_ExpectedMappingCount);
	Scene.Player->SetupPlayerInputComponent(Input);
	TestEqual(TEXT("the repeated input setup did not duplicate mappings (still 18)"),
		Mapping->GetMappings().Num(), M2_004_ExpectedMappingCount);

	// Controls work again: a submitted Light starts a real attack instance.
	Scene.Player->SubmitCombatInput(ECombatInput::Light);
	if (!Scene.WaitFor(*this, [&Scene]() { return Scene.PlayerStartedCount > 0; },
		TEXT("the revived pawn starts an attack again"), 1.0))
	{
		return true;
	}

	// Second lifecycle: the revived pawn can be killed again, and the second
	// death broadcasts PlayerDied a second time (one per lifecycle).
	TestTrue(TEXT("the health pool accepts the second lethal damage"),
		Scene.PlayerHealth->ApplyDamage(9999.0f) > 0.0f);
	TestEqual(TEXT("PlayerDied broadcast once more for the second lifecycle"), Scene.PlayerDiedCount, 2);
	TestTrue(TEXT("the combat component is dead again after the second lethal hit"),
		Scene.Player->GetCombat()->IsDead());
	return true;
}

// Acceptance: same faction never hurts itself. Two melee enemies stand
// inside one light_01 reach; enemy A fires a real attack instance through
// its own combat component and the shared pipeline must refuse the
// same-kind neighbor in the fullest sense: no damage, no stun, no event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_004WorldEnemyAttackNeverDamagesFellowEnemy,
	"UEMMO.Tasks.M2_004.WorldEnemyAttackNeverDamagesFellowEnemy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_004WorldEnemyAttackNeverDamagesFellowEnemy::RunTest(const FString& Parameters)
{
	FM2_004_Scene Scene;
	// Playerless enemy scene: the same-team evidence must come from the
	// friendly-fire refusal alone. The scene's enemy is the attacker; the
	// second enemy is spawned next to it, inside the light_01 forward reach
	// (the M2-003 suite proved the box reaches a body 100 cm ahead; 80 cm
	// keeps the capsules apart).
	if (!Scene.Build(*this, FVector(0.0f, 0.0f, 0.0f), /*bWithPlayer*/ false))
	{
		return true;
	}
	AMeleeEnemy* Fellow = M2_004_SpawnEnemy(*Scene.World, FVector(80.0f, 0.0f, 0.0f));
	if (!TestNotNull(TEXT("the fellow enemy spawns"), Fellow))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	const float FellowHpBefore = Fellow->GetHealthComponent()->GetHealth();
	const float AttackerHpBefore = Scene.Enemy->GetHealthComponent()->GetHealth();

	// No chase target anywhere: the attack starts directly on the attacker's
	// own combat component (the shared M1 pipeline, as the controller would).
	if (!TestTrue(TEXT("the attacker starts light_01"),
		Scene.EnemyCombat->TryStartAttack(M2_004_AttackId, 1)))
	{
		return true;
	}
	if (!Scene.WaitFor(*this, [&Scene]()
	{
		return Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free;
	}, TEXT("the attack instance ran out"), M2_004_MaxInstanceWaitSeconds))
	{
		return true;
	}

	TestEqual(FString::Printf(TEXT("the fellow enemy kept its HP (before %.1f, after %.1f)"),
		FellowHpBefore, Fellow->GetHealthComponent()->GetHealth()),
		Fellow->GetHealthComponent()->GetHealth(), FellowHpBefore, 0.01f);
	TestEqual(TEXT("the attacker hurt no one (no hit confirmed at all)"), Scene.EnemyHits, 0);
	TestEqual(FString::Printf(TEXT("the attacker kept its own HP (before %.1f, after %.1f)"),
		AttackerHpBefore, Scene.Enemy->GetHealthComponent()->GetHealth()),
		Scene.Enemy->GetHealthComponent()->GetHealth(), AttackerHpBefore, 0.01f);
	TestTrue(TEXT("the fellow enemy was never stunned by the friendly attack"),
		Fellow->GetCombatComponent()->GetSnapshot().ActionState != ECombatActionState::HitStun);
	return true;
}

#endif
