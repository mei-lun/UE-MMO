// M2-011: the room exit procedure and the map-unload cleanup. LeaveRoom is
// the real exit behind the M2-006 Exiting state: from Running/Failed/Cleared
// it moves the session into Exiting, cancels the still-spawning wave
// (CancelWave: future births only), drops the run's player-death binding and
// clears every per-run weak reference (alive ids, registered enemy actors,
// the per-wave spawners with their pending births). An exit is NOT a
// settlement: OnRunEnded never fires for it, so repeated LeaveRoom calls are
// idempotent (no crash, no second settlement), stale old-run enemy deaths
// arriving after the exit are ignored, and StartRoom is accepted again from
// Exiting so the player can re-enter the room and start a fresh run with new
// RunId / SettlementId. Late wave callbacks verify the wave's registered
// world and abort/ignore on a mismatch (map switch never lets an old wave act
// on a new or torn-down world), and the world-destroy path (Deinitialize)
// stays idempotent with a rebuilt world starting fully independent.
//
// The suite reuses the M2-010 RoomRetryTests scaffold precedent (a real
// BeginPlay-initialized temp game world with the production
// URoomSessionSubsystem, a real floor, real ticks and the shipped-health
// prototype pawn; the injected session clock drives the wave progression the
// way a game frame driver would). The room definition is a hand-built double
// of the room_training_01 shape (2+3 waves), the M2-007/M2-008/M2-010
// precedent for runtime definitions.
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/WaveSpawner.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_011
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M2_011_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M2_011_SceneBase(68000.0, 71000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_011_FloorHalfThickness = 100.0f;
	const float M2_011_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top.
	const float M2_011_PlayerSpawnHeight = 120.0f;
	const float M2_011_EnemySpawnHeight = 90.0f;

	// World-static blocking box (the floor shares the M2-010 builder shape).
	static AActor* M2_011_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
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
		Box->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top (the M2-004 rig: the
	// pawn carries its own UHealthComponent from spawn; no runtime attachment).
	static APrototypeCharacter* M2_011_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M2_011_SceneBase + FVector(0.0, 0.0, M2_011_PlayerSpawnHeight),
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

	// Spawns one melee enemy directly (test-owned actors: foreign scene
	// enemies are born here, NEVER through the wave spawner).
	static AMeleeEnemy* M2_011_SpawnForeignEnemy(UWorld& World, const FVector& Offset)
	{
		FActorSpawnParameters Params;
		return World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(),
			M2_011_SceneBase + Offset + FVector(0.0, 0.0, M2_011_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
	}

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-008/M2-010 suite precedent: the progression takes the definition as
	// a parameter; the JSON-to-asset loading belongs to the catalog task).
	static UEnemyDefinition* M2_011_MakeEnemyDef()
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = FName(TEXT("melee_grunt"));
		Def->MaxHP = 60.0f;
		Def->AttackPower = 0.0f;
		Def->MoveSpeed = 220.0f;
		Def->AttackRangeX = 160.0f;
		Def->AlignYTolerance = 35.0f;
		Def->TelegraphSeconds = 0.35f;
		Def->SpawnGraceSeconds = 0.5f;
		Def->MeleeAttackId = FName(TEXT("light_01"));
		return Def;
	}

	// Hand-built definition double of the room_training_01 shape (2+3 waves,
	// the M2-007/M2-008/M2-010 precedent): wave 0 two enemies, wave 1 three
	// enemies, all inside the floor rectangle of this suite's arena.
	static URoomDefinition* M2_011_MakeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m2_011_exit"));
		Room->RewardTableId = FName(TEXT("reward_m2_011"));

		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 2;
		Wave0.SpawnLocations.Add(M2_011_SceneBase + FVector(-300.0, 0.0, M2_011_EnemySpawnHeight));
		Wave0.SpawnLocations.Add(M2_011_SceneBase + FVector(300.0, 0.0, M2_011_EnemySpawnHeight));
		Room->Waves.Add(Wave0);

		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = FName(TEXT("melee_grunt"));
		Wave1.Count = 3;
		Wave1.SpawnLocations.Add(M2_011_SceneBase + FVector(-500.0, 0.0, M2_011_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(M2_011_SceneBase + FVector(0.0, 0.0, M2_011_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(M2_011_SceneBase + FVector(500.0, 0.0, M2_011_EnemySpawnHeight));
		Room->Waves.Add(Wave1);
		return Room;
	}

	// Number of ALIVE melee enemies in the world (corpses not counted).
	static int32 M2_011_CountAliveEnemies(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			UHealthComponent* Health = It->GetHealthComponent();
			if (Health != nullptr && Health->IsAlive())
			{
				++Count;
			}
		}
		return Count;
	}

	// Number of melee enemies in the world INCLUDING corpses.
	static int32 M2_011_CountAllEnemies(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	// First ALIVE melee enemy standing at the given world-space location (the
	// alive-only filter keeps a corpse from shadowing a fresh enemy born at
	// the same configured location - the M2-008/M2-010 precedent).
	static AMeleeEnemy* M2_011_FindAliveEnemyAt(UWorld& World, const FVector& Location)
	{
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			UHealthComponent* Health = It->GetHealthComponent();
			if (Health == nullptr || !Health->IsAlive())
			{
				continue;
			}
			if (FVector::DistSquared(It->GetActorLocation(), Location) < 1.0)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// Kills one enemy through its health component and verifies the death took.
	static bool M2_011_KillEnemy(FAutomationTestBase& Test, AMeleeEnemy* Enemy, const TCHAR* What)
	{
		if (!Test.TestNotNull(What, Enemy))
		{
			return false;
		}
		Enemy->GetHealthComponent()->ApplyDamage(999.0f);
		return Test.TestTrue(TEXT("the enemy died from the applied damage"),
			Enemy->GetHealthComponent() != nullptr && !Enemy->GetHealthComponent()->IsAlive());
	}

	// One temp game world with its real session subsystem, a real floor and
	// the shipped-health prototype pawn. The world ticks for real; the wave
	// progression is driven by injecting the session clock alongside every
	// tick (the per-frame duty a game frame driver would perform).
	struct FM2_011_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		double ClockSeconds = 0.0;

		// Recorded session and pawn events (the assertions read these only).
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;
		int32 PlayerDiedCount = 0;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created (engine FTestWorldWrapper precedent)"),
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
			Session->OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
			});
			Session->OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});

			AActor* Floor = M2_011_SpawnBoxActor(*World,
				M2_011_SceneBase - FVector(0.0, 0.0, M2_011_FloorHalfThickness),
				FVector(M2_011_FloorHalfExtentXY, M2_011_FloorHalfExtentXY, M2_011_FloorHalfThickness),
				TEXT("M2_011_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first tick (the M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			Player = M2_011_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Player->PlayerDied.AddLambda([this]()
			{
				++PlayerDiedCount;
			});
			// The death handler binds here (LeaveRoom must drop it again).
			Session->SetPlayer(Player);
			return true;
		}

		/** Ticks the world once with the requested delta. */
		bool TickSeconds(FAutomationTestBase& Test, float DeltaSeconds)
		{
			return Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(DeltaSeconds));
		}

		/** Injects one exact clock value (wave anchors read exact instants). */
		void InjectClock(double NowSeconds)
		{
			ClockSeconds = NowSeconds;
			Session->SetSessionClockSeconds(NowSeconds);
		}

		/**
		 * Advances the world with real 60 fps ticks while injecting the
		 * session clock alongside (the same per-frame duty a game driver
		 * performs), so due births and the 1.0 s inter-wave wait elapse
		 * through the production pump path.
		 */
		bool AdvanceSeconds(FAutomationTestBase& Test, double Seconds)
		{
			const double StepSeconds = static_cast<double>(M2_011_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				InjectClock(ClockSeconds + StepNow);
				if (!TickSeconds(Test, M2_011_FrameSeconds))
				{
					return false;
				}
				Remaining -= StepNow;
			}
			return true;
		}

		/** One lethal hit through the pawn's own health pool. */
		bool KillPlayer(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the health pool accepts the lethal damage"),
				Player->GetHealth()->ApplyDamage(9999.0f) > 0.0f))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the health pool is dead after the lethal damage"),
				!Player->GetHealth()->IsAlive());
		}
	};
}

using namespace UE::UEMMO::Tasks::M2_011;

// 1. Acceptance: leaving mid wave-0 GENERATION (one enemy born, one unborn)
//    zeroes every pending birth of the run immediately, and neither a
//    far-future clock injection nor real world ticks ever produce a new
//    enemy. The exit fires NO end event (an exit is not a settlement), and a
//    player death broadcast after the exit changes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_011LeaveRoomMidSpawningZeroesPendingAndStopsBirths,
	"UEMMO.Tasks.M2_011.LeaveRoomMidSpawningZeroesPendingAndStopsBirths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_011LeaveRoomMidSpawningZeroesPendingAndStopsBirths::RunTest(const FString& Parameters)
{
	FM2_011_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_011_MakeRoom();
	UEnemyDefinition* Def = M2_011_MakeEnemyDef();

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the progression begins from wave 0"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	TestEqual(TEXT("the anchor injection birthed the first wave-0 enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("the wave is mid-generation (one enemy still unborn)"),
		Scene.Session->GetRunPendingSpawnCount(), 1);
	TestTrue(TEXT("the run is Running before the leave"),
		Scene.Session->GetState() == ERoomSessionState::Running);

	if (!TestTrue(TEXT("LeaveRoom accepts the exit from the Running run"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Exiting after the leave"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the unborn spawns were cleared with the room (PendingSpawns 0)"),
		Scene.Session->GetRunPendingSpawnCount(), 0);
	TestEqual(TEXT("the exit fired no end event (an exit is not a settlement)"), Scene.EndedCount, 0);
	TestEqual(TEXT("the exit fired no extra start event"), Scene.StartedCount, 1);

	// A far-future injection would birth a leaked pending slot or fire a
	// leaked inter-wave wait; the cancelled run must produce nothing.
	Scene.InjectClock(10.0);
	TestEqual(TEXT("the far-future injection birthed no new enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("the far-future injection left no pending spawns"),
		Scene.Session->GetRunPendingSpawnCount(), 0);

	// Real world ticks change nothing: the cancelled wave can never resume.
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	TestEqual(TEXT("no new spawn happened during the 2 s ticked wait"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("the world holds exactly the one pre-leave enemy"),
		M2_011_CountAllEnemies(*Scene.World), 1);
	TestTrue(TEXT("the session stays Exiting through the ticks"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("still no end event after the ticks"), Scene.EndedCount, 0);

	// A player death broadcast after the exit must not fail, settle or
	// disturb anything (the run-scoped death binding was dropped).
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestEqual(TEXT("the pawn broadcast its own death lifecycle"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the stale death left the session Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the stale death fired no end event"), Scene.EndedCount, 0);
	return true;
}

// 2. Acceptance: three consecutive LeaveRoom calls (and a fourth after stale
//    events) never crash, never change the Exiting state and never produce a
//    second settlement - OnRunEnded stays silent through the whole exit flow,
//    and kill notifications while Exiting are refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_011RepeatedLeaveRoomIsIdempotentAndNeverSettles,
	"UEMMO.Tasks.M2_011.RepeatedLeaveRoomIsIdempotentAndNeverSettles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_011RepeatedLeaveRoomIsIdempotentAndNeverSettles::RunTest(const FString& Parameters)
{
	FM2_011_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_011_MakeRoom();
	UEnemyDefinition* Def = M2_011_MakeEnemyDef();

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	TestEqual(TEXT("one wave-0 enemy was born before the leave"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("one enemy is still unborn before the leave"),
		Scene.Session->GetRunPendingSpawnCount(), 1);

	// Three consecutive leaves: the first exits, the repeats are idempotent.
	for (int32 Attempt = 1; Attempt <= 3; ++Attempt)
	{
		if (!TestTrue(FString::Printf(TEXT("LeaveRoom call %d succeeded (idempotent)"), Attempt),
			Scene.Session->LeaveRoom()))
		{
			return true;
		}
		TestTrue(FString::Printf(TEXT("after call %d the session is Exiting"), Attempt),
			Scene.Session->GetState() == ERoomSessionState::Exiting);
		TestEqual(FString::Printf(TEXT("after call %d no pending spawns remain"), Attempt),
			Scene.Session->GetRunPendingSpawnCount(), 0);
		TestEqual(FString::Printf(TEXT("after call %d no end event fired"), Attempt),
			Scene.EndedCount, 0);
		TestEqual(FString::Printf(TEXT("after call %d no extra start event fired"), Attempt),
			Scene.StartedCount, 1);
	}

	// Stale events after the repeats: a far-future injection and a direct
	// kill notification while Exiting change nothing.
	Scene.InjectClock(100.0);
	TestEqual(TEXT("the far-future injection birthed no new enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestTrue(TEXT("the kill notification while Exiting is refused"),
		!Scene.Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s1"))));
	TestEqual(TEXT("the refused notification counted no kill"), Scene.Session->GetKilledCount(), 0);

	// A fourth leave after the stale events: still idempotent, still silent.
	TestTrue(TEXT("a repeated LeaveRoom after stale events still succeeds"),
		Scene.Session->LeaveRoom());
	TestTrue(TEXT("the session is still Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("still exactly zero end events (no settlement ever)"), Scene.EndedCount, 0);
	return true;
}

// 3. Acceptance: after an exit the room can be re-entered: StartRoom from
//    Exiting starts a fresh run (new RunId / SettlementId / Seed), the wave
//    progression restarts from wave 0 and the fresh run clears normally.
//    Both post-terminal exits are covered: from Failed and from Cleared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_011AfterExitStartRoomStartsFreshRunWithNewIds,
	"UEMMO.Tasks.M2_011.AfterExitStartRoomStartsFreshRunWithNewIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_011AfterExitStartRoomStartsFreshRunWithNewIds::RunTest(const FString& Parameters)
{
	FM2_011_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_011_MakeRoom();
	UEnemyDefinition* Def = M2_011_MakeEnemyDef();

	// Run A: fails mid wave-0 generation (the M2-010 failure path).
	if (!TestTrue(TEXT("run A starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	const uint64 OldSettlementId = Scene.Session->GetSettlementId();
	const int32 OldSeed = Scene.Session->GetSeed();
	if (!TestTrue(TEXT("run A's progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	if (!TestTrue(TEXT("run A fails mid-generation"), Scene.Session->FailRun()))
	{
		return true;
	}
	TestTrue(TEXT("run A is Failed"), Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("run A's failure fired exactly one end event"), Scene.EndedCount, 1);

	// Leaving from Failed: Exiting, no re-settlement.
	if (!TestTrue(TEXT("LeaveRoom accepts the exit from the Failed run"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Exiting after leaving the Failed run"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the exit fired no second end event"), Scene.EndedCount, 1);

	// Run A's wave-0 leftover is still alive in the world (the exit cleared
	// the run's weak references WITHOUT destroying actors). Kill it while the
	// session is Exiting: a dead run's death must count and settle nothing -
	// and this keeps the arena unambiguous for run B's location lookups.
	AMeleeEnemy* RunALeftover = M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]);
	if (!M2_011_KillEnemy(*this, RunALeftover, TEXT("run A's wave-0 leftover was killed while Exiting")))
	{
		return true;
	}
	TestEqual(TEXT("the leftover death while Exiting counted no kill"),
		Scene.Session->GetKilledCount(), 0);
	TestTrue(TEXT("the leftover death while Exiting left the session Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the leftover death while Exiting fired no end event"), Scene.EndedCount, 1);

	// Run B: starts straight from Exiting with completely fresh identities.
	if (!TestTrue(TEXT("StartRoom from Exiting starts a fresh run"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("run B got a new, larger RunId"), NewRunId > OldRunId);
	TestTrue(TEXT("run B got a new, larger SettlementId"),
		Scene.Session->GetSettlementId() > OldSettlementId);
	TestTrue(TEXT("run B derived a new seed from the new RunId"),
		Scene.Session->GetSeed() != OldSeed);
	TestTrue(TEXT("run B is Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("exactly one start event per accepted run"), Scene.StartedCount, 2);
	TestEqual(TEXT("run B counts no old kills"), Scene.Session->GetKilledCount(), 0);
	TestEqual(TEXT("run B counts no old spawns"), Scene.Session->GetSpawnedEnemyCount(), 0);

	// Run B re-runs the full 2+3 progression and clears normally. The births
	// and kills use exact clock injections WITHOUT world ticks (the M2-008
	// precedent: the progression is pure bookkeeping on the injected clock),
	// so every enemy stays exactly on its configured spawn location for the
	// location-based lookups below.
	if (!TestTrue(TEXT("run B's progression begins from wave 0"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	Scene.InjectClock(0.35);
	TestEqual(TEXT("run B's wave 0 birthed exactly its two configured enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
			TEXT("run B's first wave-0 enemy was found"))
		|| !M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
			TEXT("run B's second wave-0 enemy was found")))
	{
		return true;
	}
	Scene.InjectClock(1.4);
	Scene.InjectClock(1.75);
	Scene.InjectClock(2.1);
	TestEqual(TEXT("run B's wave 1 birthed exactly its three configured enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[0]),
			TEXT("run B's first wave-1 enemy was found"))
		|| !M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[1]),
			TEXT("run B's second wave-1 enemy was found"))
		|| !M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[2]),
			TEXT("run B's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("run B cleared normally"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("run B's clear fired its own end event"), Scene.EndedCount, 2);
	if (Scene.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("run B's result is Cleared"), Scene.EndedResults[1].bCleared);
		TestEqual(TEXT("run B's result carries the new run id"),
			Scene.EndedResults[1].RunId, NewRunId);
	}

	// Leaving from Cleared: Exiting again, the recorded result stays settled.
	if (!TestTrue(TEXT("LeaveRoom accepts the exit from the Cleared run"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Exiting after leaving the Cleared run"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the exit fired no second end event for the Cleared run"), Scene.EndedCount, 2);
	TestEqual(TEXT("the Cleared result was never overwritten by the exit"),
		Scene.Session->GetLastResult().RunId, NewRunId);

	// Run C: one more fresh run from Exiting proves the loop can continue.
	if (!TestTrue(TEXT("a second StartRoom from Exiting starts run C"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("run C got a newer, larger RunId"), Scene.Session->GetRunId() > NewRunId);
	if (!TestTrue(TEXT("run C's progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(3.0);
	TestEqual(TEXT("run C's wave 0 started birthing again"),
		Scene.Session->GetSpawnedEnemyCount(), 1);

	// The fresh run C is left as well; still no settlement from an exit.
	TestTrue(TEXT("run C is left too"), Scene.Session->LeaveRoom());
	TestTrue(TEXT("the session is Exiting at the end"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("three lifecycles produced exactly two end events (fails + clears only)"),
		Scene.EndedCount, 2);
	TestEqual(TEXT("three lifecycles produced exactly three start events"),
		Scene.StartedCount, 3);
	return true;
}

// 4. Acceptance: old-run enemy deaths arriving after LeaveRoom are ignored -
//    a leftover dying while the session is Exiting changes nothing, a stale
//    direct notification is refused, and a leftover dying during a LATER new
//    run is swallowed by the spawner's run-id filter before it can touch the
//    new run's bookkeeping (which then completes its own progression).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_011OldRunEnemyDeathAfterLeaveRoomIsIgnored,
	"UEMMO.Tasks.M2_011.OldRunEnemyDeathAfterLeaveRoomIsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_011OldRunEnemyDeathAfterLeaveRoomIsIgnored::RunTest(const FString& Parameters)
{
	FM2_011_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_011_MakeRoom();
	UEnemyDefinition* Def = M2_011_MakeEnemyDef();

	// Run A: one enemy born, then the player leaves mid-generation.
	if (!TestTrue(TEXT("run A starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("run A's progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	AMeleeEnemy* OldBornA = M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]);
	if (!TestTrue(TEXT("LeaveRoom accepts the mid-generation exit"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	// The leave clears the run's weak references without destroying actors.
	TestTrue(TEXT("the old run's born enemy survived the leave (weak refs only)"),
		IsValid(OldBornA));

	// Part 1: the old-run death arrives while the session is Exiting.
	if (!M2_011_KillEnemy(*this, OldBornA, TEXT("the old run's born enemy was killed while Exiting")))
	{
		return true;
	}
	TestEqual(TEXT("the old-run death while Exiting counted no kill"),
		Scene.Session->GetKilledCount(), 0);
	TestTrue(TEXT("the old-run death while Exiting left the session Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the old-run death while Exiting fired no end event"), Scene.EndedCount, 0);
	TestTrue(TEXT("a direct stale kill notification while Exiting is refused"),
		!Scene.Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s1"))));
	TestEqual(TEXT("the refused notification counted no kill"), Scene.Session->GetKilledCount(), 0);

	// Part 2: a leftover of run B dying during the NEW run C.
	if (!TestTrue(TEXT("run B starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("run B's progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(50.0);
	AMeleeEnemy* OldBornB = M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]);
	if (!TestNotNull(TEXT("run B's first wave-0 enemy was born"), OldBornB))
	{
		return true;
	}
	if (!TestTrue(TEXT("run B is left mid-generation too"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	TestTrue(TEXT("run B's born enemy survived run B's leave"), IsValid(OldBornB));

	if (!TestTrue(TEXT("run C starts from the Exiting session"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("run C's progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	// Exact clock injections, no world ticks (see run B): every enemy stays
	// exactly on its configured spawn location for the lookups below.
	Scene.InjectClock(100.0);
	Scene.InjectClock(100.35);
	TestEqual(TEXT("run B's wave 0 birthed exactly its two configured enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);

	// The run B leftover dies during run C: the spawner's run-id filter
	// swallows it before it can reach run C's kill bookkeeping.
	if (!M2_011_KillEnemy(*this, OldBornB, TEXT("run B's leftover was killed during run C")))
	{
		return true;
	}
	TestEqual(TEXT("the old-run death during run C entered no kill into run C"),
		Scene.Session->GetKilledCount(), 0);
	TestTrue(TEXT("run C is still Running after the old-run death"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the old-run death fired no end event"), Scene.EndedCount, 0);

	// Run C is healthy: its own wave 0 settles (the alive-only filter skips
	// the corpse of run B's leftover at the same configured location).
	if (!M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
			TEXT("run C's first wave-0 enemy was found"))
		|| !M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
			TEXT("run C's second wave-0 enemy was found")))
	{
		return true;
	}
	Scene.InjectClock(101.4);
	Scene.InjectClock(101.75);
	Scene.InjectClock(102.1);
	TestEqual(TEXT("run C's wave 1 birthed exactly its three configured enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[0]),
			TEXT("run C's first wave-1 enemy was found"))
		|| !M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[1]),
			TEXT("run C's second wave-1 enemy was found"))
		|| !M2_011_KillEnemy(*this, M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[2]),
			TEXT("run C's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("run C cleared despite the old-run leftovers"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("run C's result counts exactly its own five real deaths"),
		Scene.Session->GetLastResult().KilledCount, 5);
	TestEqual(TEXT("exactly one end event in the whole scene (run C's clear)"),
		Scene.EndedCount, 1);
	return true;
}

// 5. Acceptance: the world teardown cleanup is idempotent. Calling
//    Deinitialize (the EndPlay path) after an exit cleans everything and is
//    safe to call twice; a test-owned spawner's late birth callback against
//    the torn-down world aborts into Failed instead of spawning into it; and
//    a rebuilt world's fresh session starts fully independent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_011WorldTeardownCleanupIsIdempotentAndRebuiltWorldIsIndependent,
	"UEMMO.Tasks.M2_011.WorldTeardownCleanupIsIdempotentAndRebuiltWorldIsIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_011WorldTeardownCleanupIsIdempotentAndRebuiltWorldIsIndependent::RunTest(const FString& Parameters)
{
	// Phase 1: Deinitialize after an exit cleans everything, twice.
	FM2_011_Scene SceneA;
	if (!SceneA.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_011_MakeRoom();
	UEnemyDefinition* Def = M2_011_MakeEnemyDef();
	if (!TestTrue(TEXT("run A starts"), SceneA.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("run A's progression begins"), SceneA.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	SceneA.InjectClock(0.0);
	if (!TestTrue(TEXT("run A is left mid-generation"), SceneA.Session->LeaveRoom()))
	{
		return true;
	}
	// The simulated EndPlay cleanup (the world-destroy path calls exactly
	// this); called twice to prove the cleanup is idempotent.
	SceneA.Session->Deinitialize();
	SceneA.Session->Deinitialize();
	TestTrue(TEXT("after the double Deinitialize the session is Idle"),
		SceneA.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the cleanup cleared the run id"), SceneA.Session->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("the cleanup cleared the settlement id"),
		SceneA.Session->GetSettlementId(), static_cast<uint64>(0));
	TestEqual(TEXT("the cleanup cleared the pending spawns"),
		SceneA.Session->GetRunPendingSpawnCount(), 0);
	TestEqual(TEXT("the cleanup cleared the spawn bookkeeping"),
		SceneA.Session->GetSpawnedEnemyCount(), 0);
	TestFalse(TEXT("the cleanup dropped the end-event binding"),
		SceneA.Session->OnRunEnded().IsBound());
	TestFalse(TEXT("the cleanup dropped the start-event binding"),
		SceneA.Session->OnRunStarted().IsBound());

	// Phase 2: the REAL world-destroy path with a late birth callback. A
	// test-owned spawner drives wave 1 of the running run; its registered
	// world is torn down before its births come due.
	FM2_011_Scene SceneB;
	if (!SceneB.Build(*this))
	{
		return true;
	}
	if (!TestTrue(TEXT("run B starts"), SceneB.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("run B's progression begins"), SceneB.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	SceneB.InjectClock(0.0);
	UWaveSpawner* OwnedSpawner = NewObject<UWaveSpawner>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestTrue(TEXT("the test-owned spawner accepted wave 1"),
		OwnedSpawner->StartWave(Room, 1, Def, SceneB.Session) == EWaveStartResult::Started))
	{
		return true;
	}
	TestEqual(TEXT("the owned wave 1 holds its three unborn spawns"),
		OwnedSpawner->GetPendingSpawnCount(), 3);

	// Destroy without a forced GC: the world is torn down synchronously
	// (EndPlay marks it tearing down) while the objects are still in memory,
	// which is exactly the late-callback window the guard exists for.
	if (!TestTrue(TEXT("the test world B is destroyed"),
		SceneB.Wrapper.DestroyTestWorld(/*bForceGarbageCollect*/ false)))
	{
		return true;
	}
	// The late birth callback: must abort the wave (never spawn into the
	// torn-down world) instead of crashing or misbehaving.
	OwnedSpawner->UpdateWave(1000.0);
	TestTrue(TEXT("the late birth callback aborted the wave into Failed"),
		OwnedSpawner->GetState() == EWaveState::Failed);
	TestEqual(TEXT("the aborted wave kept no pending spawns"),
		OwnedSpawner->GetPendingSpawnCount(), 0);
	TestTrue(TEXT("the abort kept a diagnostic"),
		!OwnedSpawner->GetLastFailure().IsEmpty());

	// Phase 3: a rebuilt world's fresh session starts fully independent.
	FM2_011_Scene SceneC;
	if (!SceneC.Build(*this))
	{
		return true;
	}
	TestTrue(TEXT("the rebuilt world's session starts Idle"),
		SceneC.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the rebuilt world's session has no run id"),
		SceneC.Session->GetRunId(), static_cast<uint64>(0));
	if (!TestTrue(TEXT("the rebuilt world starts its own run"), SceneC.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the rebuilt world's run is Running"),
		SceneC.Session->GetState() == ERoomSessionState::Running);
	TestTrue(TEXT("the rebuilt world's run got a fresh settlement id"),
		SceneC.Session->GetSettlementId() > 0);
	if (!TestTrue(TEXT("the rebuilt world's progression begins"),
		SceneC.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	SceneC.InjectClock(0.0);
	TestEqual(TEXT("the rebuilt world's wave 0 births normally"),
		SceneC.Session->GetSpawnedEnemyCount(), 1);
	return true;
}

// 6. Acceptance: leaving mid-generation does no friendly fire - the born
//    enemy of the run, a foreign melee enemy spawned outside the spawner and
//    a plain non-enemy scene actor all survive the exit and the following
//    far-future injections untouched (the leave clears weak references, it
//    never destroys or kills actors).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_011LeaveRoomMidSpawningDoesNotHarmOtherSceneActors,
	"UEMMO.Tasks.M2_011.LeaveRoomMidSpawningDoesNotHarmOtherSceneActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_011LeaveRoomMidSpawningDoesNotHarmOtherSceneActors::RunTest(const FString& Parameters)
{
	FM2_011_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_011_MakeRoom();
	UEnemyDefinition* Def = M2_011_MakeEnemyDef();

	// Foreign scene actors born OUTSIDE the spawner, before any run existed.
	AMeleeEnemy* Bystander = M2_011_SpawnForeignEnemy(*Scene.World, FVector(0.0, -600.0, 0.0));
	if (!TestNotNull(TEXT("the foreign bystander enemy spawns outside the spawner"), Bystander))
	{
		return true;
	}
	AActor* PlainProp = M2_011_SpawnBoxActor(*Scene.World,
		M2_011_SceneBase + FVector(0.0, 600.0, 100.0), FVector(50.0f, 50.0f, 50.0f),
		TEXT("M2_011_PlainProp"));
	if (!TestNotNull(TEXT("the plain non-enemy scene actor spawns"), PlainProp))
	{
		return true;
	}

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	AMeleeEnemy* BornEnemy = M2_011_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]);
	if (!TestNotNull(TEXT("the run's first wave-0 enemy was born"), BornEnemy))
	{
		return true;
	}

	if (!TestTrue(TEXT("LeaveRoom accepts the mid-generation exit"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	Scene.InjectClock(10.0);
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}

	// Every pre-leave actor is untouched: the run's own born enemy (the leave
	// clears weak references WITHOUT destroying), the foreign enemy and the
	// plain prop; and no new enemy ever appeared.
	TestTrue(TEXT("the run's born enemy is still a valid actor after the leave"),
		IsValid(BornEnemy));
	if (BornEnemy != nullptr)
	{
		TestTrue(TEXT("the run's born enemy is still alive (the leave never kills)"),
			BornEnemy->GetHealthComponent() != nullptr && BornEnemy->GetHealthComponent()->IsAlive());
	}
	TestTrue(TEXT("the foreign bystander is still a valid actor after the leave"),
		IsValid(Bystander));
	if (Bystander != nullptr)
	{
		TestTrue(TEXT("the foreign bystander is still alive after the leave"),
			Bystander->GetHealthComponent() != nullptr && Bystander->GetHealthComponent()->IsAlive());
	}
	TestTrue(TEXT("the plain non-enemy scene actor survived the leave"), IsValid(PlainProp));
	TestEqual(TEXT("the world holds exactly the born enemy and the bystander"),
		M2_011_CountAllEnemies(*Scene.World), 2);
	TestEqual(TEXT("the alive count holds both enemies"),
		M2_011_CountAliveEnemies(*Scene.World), 2);
	TestEqual(TEXT("no new spawn happened after the leave"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	return true;
}

#endif
