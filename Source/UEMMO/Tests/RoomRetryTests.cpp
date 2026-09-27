// M2-010: player failure and single-run retry. A registered player pawn's
// PlayerDied broadcast (M2-004, one per death lifecycle) fails the Running run
// (first terminal wins), cancels the current wave's unborn spawns and stops -
// never kills - every enemy the run registered. URoomRetryService::RetryRoom
// then restarts a FAILED run in one call: it destroys ONLY the enemies the
// failed run registered (never a world scan of Characters), resets the
// session, revives the pawn through the M2-004 reset entry and starts a fresh
// run with a new RunId / SettlementId plus a restarted progression - with no
// per-retry accumulation, so the Nth retry is exactly like the first.
//
// The suite reuses the M2-006/M2-007/M2-008 FTestWorldWrapper precedent (a
// real BeginPlay-initialized temp game world with the production
// URoomSessionSubsystem) and the M2-004 world rig (real floor, real ticks,
// the shipped-health prototype pawn). The progression itself runs on the
// injected session clock exactly like the production flow; the driver here
// injects it per tick the way a game frame driver would. The room definition
// is a hand-built double of the room_training_01 shape (2+3 waves), the
// M2-007/M2-008 suite precedent for runtime definitions.
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomRetryService.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_010
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M2_010_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M2_010_SceneBase(59000.0, 52000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_010_FloorHalfThickness = 100.0f;
	const float M2_010_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top.
	const float M2_010_PlayerSpawnHeight = 120.0f;
	const float M2_010_EnemySpawnHeight = 90.0f;

	// World-static blocking box (the floor shares the M2-004 builder shape).
	static AActor* M2_010_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
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
	static APrototypeCharacter* M2_010_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M2_010_SceneBase + FVector(0.0, 0.0, M2_010_PlayerSpawnHeight),
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

	// Spawns one melee enemy directly (test-owned actors: the foreign enemy of
	// the destruction-scope test is born here, NEVER through the wave spawner).
	static AMeleeEnemy* M2_010_SpawnEnemyAt(UWorld& World, const FVector& Offset)
	{
		FActorSpawnParameters Params;
		return World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(),
			M2_010_SceneBase + Offset + FVector(0.0, 0.0, M2_010_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
	}

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-008 suite precedent: the progression takes the definition as a
	// parameter; the JSON-to-asset loading belongs to the catalog task).
	static UEnemyDefinition* M2_010_MakeEnemyDef()
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
	// the M2-007/M2-008 precedent): wave 0 two enemies, wave 1 three enemies,
	// all inside the floor rectangle of this suite's arena.
	static URoomDefinition* M2_010_MakeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m2_010_retry"));
		Room->RewardTableId = FName(TEXT("reward_m2_010"));

		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 2;
		Wave0.SpawnLocations.Add(M2_010_SceneBase + FVector(-300.0, 0.0, M2_010_EnemySpawnHeight));
		Wave0.SpawnLocations.Add(M2_010_SceneBase + FVector(300.0, 0.0, M2_010_EnemySpawnHeight));
		Room->Waves.Add(Wave0);

		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = FName(TEXT("melee_grunt"));
		Wave1.Count = 3;
		Wave1.SpawnLocations.Add(M2_010_SceneBase + FVector(-500.0, 0.0, M2_010_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(M2_010_SceneBase + FVector(0.0, 0.0, M2_010_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(M2_010_SceneBase + FVector(500.0, 0.0, M2_010_EnemySpawnHeight));
		Room->Waves.Add(Wave1);
		return Room;
	}

	// Number of ALIVE melee enemies in the world (corpses not counted).
	static int32 M2_010_CountAliveEnemies(UWorld& World)
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

	// Number of melee enemies in the world INCLUDING corpses (a fresh run's
	// world must hold only its own actors: the retry cleanup also removes the
	// failed run's bodies).
	static int32 M2_010_CountAllEnemies(UWorld& World)
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
	// the same configured location - the M2-008 precedent).
	static AMeleeEnemy* M2_010_FindAliveEnemyAt(UWorld& World, const FVector& Location)
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
	static bool M2_010_KillEnemy(FAutomationTestBase& Test, AMeleeEnemy* Enemy, const TCHAR* What)
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
	struct FM2_010_Scene
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

			AActor* Floor = M2_010_SpawnBoxActor(*World,
				M2_010_SceneBase - FVector(0.0, 0.0, M2_010_FloorHalfThickness),
				FVector(M2_010_FloorHalfExtentXY, M2_010_FloorHalfExtentXY, M2_010_FloorHalfThickness),
				TEXT("M2_010_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first tick (the M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			Player = M2_010_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Player->PlayerDied.AddLambda([this]()
			{
				++PlayerDiedCount;
			});
			// The registration under test (the death handler binds here).
			Session->SetPlayer(Player);
			return true;
		}

		/** Ticks the world once with the requested delta. */
		bool TickSeconds(FAutomationTestBase& Test, float DeltaSeconds)
		{
			return Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(DeltaSeconds));
		}

		/**
		 * Injects one exact clock value (the wave anchor and the death-driven
		 * waits read exact instants from this).
		 */
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
			const double StepSeconds = static_cast<double>(M2_010_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				InjectClock(ClockSeconds + StepNow);
				if (!TickSeconds(Test, M2_010_FrameSeconds))
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

using namespace UE::UEMMO::Tasks::M2_010;

// 1. Acceptance: the player dies while wave 0 is still GENERATING (one enemy
//    born, one unborn). The run fails immediately (first terminal wins), the
//    unborn spawns are cancelled (PendingSpawns -> 0), and waiting 2 real
//    seconds produces no new spawn anywhere. A later death broadcast in the
//    same Failed run fires no second end event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_010WaveSpawningPlayerDeathFailsRunAndStopsSpawns,
	"UEMMO.Tasks.M2_010.WaveSpawningPlayerDeathFailsRunAndStopsSpawns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_010WaveSpawningPlayerDeathFailsRunAndStopsSpawns::RunTest(const FString& Parameters)
{
	FM2_010_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_010_MakeRoom();
	UEnemyDefinition* Def = M2_010_MakeEnemyDef();

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the progression begins from wave 0"), Scene.Session->BeginWaves(Room, Def));
	Scene.InjectClock(0.0);
	TestEqual(TEXT("the anchor injection birthed the first wave-0 enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("the wave is mid-generation (one enemy still unborn)"),
		Scene.Session->GetRunPendingSpawnCount(), 1);
	TestTrue(TEXT("the run is Running before the death"),
		Scene.Session->GetState() == ERoomSessionState::Running);

	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestEqual(TEXT("PlayerDied broadcast exactly once for the death"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the player death failed the running run"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the failure fired exactly one end event"), Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records the failure"), !Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result keeps the run id"), Scene.EndedResults[0].RunId, RunId);
	}
	TestEqual(TEXT("the unborn spawns were cancelled with the run (PendingSpawns 0)"),
		Scene.Session->GetRunPendingSpawnCount(), 0);

	// The 2 s acceptance window: no new enemy appears anywhere, the failed
	// run stays settled.
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	TestEqual(TEXT("no new spawn happened during the 2 s wait"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("the world holds exactly the one pre-death enemy (no new spawn)"),
		M2_010_CountAllEnemies(*Scene.World), 1);
	TestTrue(TEXT("the run is still Failed after the 2 s wait"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("no further end event fired during the wait"), Scene.EndedCount, 1);

	// A later death in the same Failed run (fresh lifecycle via the M2-004
	// reset, which revives without broadcasting) must not re-fail anything.
	Scene.Player->ApplyTrainingRoomReset();
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestEqual(TEXT("the revived pawn broadcast its new death lifecycle"), Scene.PlayerDiedCount, 2);
	TestTrue(TEXT("the death while Failed left the run Failed"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the death while Failed fired no second end event"), Scene.EndedCount, 1);
	return true;
}

// 2. Acceptance: the player dies during wave-1 COMBAT (all five enemies of
//    the 2+3 progression born, three still alive). The run fails, the alive
//    enemies are stopped (velocity zeroed, still alive - stop is not kill,
//    not destroy), nothing further spawns during a 2 s wait and no second
//    end event fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_010WaveTwoCombatPlayerDeathFailsRunAndStopsEnemies,
	"UEMMO.Tasks.M2_010.WaveTwoCombatPlayerDeathFailsRunAndStopsEnemies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_010WaveTwoCombatPlayerDeathFailsRunAndStopsEnemies::RunTest(const FString& Parameters)
{
	FM2_010_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_010_MakeRoom();
	UEnemyDefinition* Def = M2_010_MakeEnemyDef();
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room))
		|| !TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	// Wave 0: both enemies born and killed.
	Scene.InjectClock(0.0);
	if (!Scene.AdvanceSeconds(*this, 0.35))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 enemies were born"), Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
			TEXT("the first wave-0 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
			TEXT("the second wave-0 enemy was found")))
	{
		return true;
	}
	// The 1.0 s gap elapses and wave 1 births all three of its enemies.
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	TestEqual(TEXT("wave 1 was started"), Scene.Session->GetStartedWaveCount(), 2);
	TestEqual(TEXT("all three wave-1 enemies were born (run total 2+3)"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("exactly three wave-1 enemies are alive"), M2_010_CountAliveEnemies(*Scene.World), 3);

	// One alive enemy gets a real horizontal velocity, so the stop path has an
	// observable effect (nothing else drives a controller-less enemy).
	AMeleeEnemy* Chaser = M2_010_FindAliveEnemyAt(*Scene.World, LocW1A);
	if (!TestNotNull(TEXT("the first wave-1 enemy was found for the stop probe"), Chaser))
	{
		return true;
	}
	if (UCharacterMovementComponent* Movement = Chaser->GetCharacterMovement())
	{
		Movement->Velocity = FVector(240.0f, 0.0f, 0.0f);
	}

	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestTrue(TEXT("the mid-wave-1 player death failed the run"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the failure fired exactly one end event"), Scene.EndedCount, 1);
	TestEqual(TEXT("the combat death left no unborn spawns"),
		Scene.Session->GetRunPendingSpawnCount(), 0);

	// The registered enemies were stopped, not killed and not destroyed.
	if (!Scene.TickSeconds(*this, M2_010_FrameSeconds))
	{
		return true;
	}
	if (!TestNotNull(TEXT("the stopped enemy is still a valid actor"), Chaser))
	{
		return true;
	}
	TestTrue(TEXT("the stopped enemy is still alive (stop is not kill)"),
		Chaser->GetHealthComponent() != nullptr && Chaser->GetHealthComponent()->IsAlive());
	TestTrue(TEXT("the stopped enemy's velocity was zeroed by the stop"),
		Chaser->GetCharacterMovement() != nullptr
		&& Chaser->GetCharacterMovement()->Velocity.Size2D() < 1.0f);
	TestTrue(TEXT("the stopped enemy's combat instance is not attacking"),
		Chaser->GetCombatComponent() == nullptr
		|| Chaser->GetCombatComponent()->GetSnapshot().ActionState != ECombatActionState::Attacking);

	// The 2 s acceptance window after the combat death: no new spawn anywhere.
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	TestEqual(TEXT("no new spawn happened during the 2 s wait"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("the world holds exactly the five pre-death enemies"),
		M2_010_CountAllEnemies(*Scene.World), 5);
	TestEqual(TEXT("no further end event fired during the wait"), Scene.EndedCount, 1);
	return true;
}

// 3. Acceptance: RetryRoom after a wave-0 generation death destroys ONLY the
//    failed run's registered enemies (the foreign test-owned enemy survives),
//    starts a fresh run with new RunId / SettlementId / Seed, and the new run
//    re-runs the full 2+3 progression with exactly the configured counts and
//    clears normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_010RetryDestroysRunEnemiesAndStartsFreshRun,
	"UEMMO.Tasks.M2_010.RetryDestroysRunEnemiesAndStartsFreshRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_010RetryDestroysRunEnemiesAndStartsFreshRun::RunTest(const FString& Parameters)
{
	FM2_010_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_010_MakeRoom();
	UEnemyDefinition* Def = M2_010_MakeEnemyDef();

	if (!TestTrue(TEXT("the first run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	const uint64 OldSettlementId = Scene.Session->GetSettlementId();
	const int32 OldSeed = Scene.Session->GetSeed();
	if (!TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	// A foreign enemy the run never registered: the retry must not touch it.
	AMeleeEnemy* Foreign = M2_010_SpawnEnemyAt(*Scene.World, FVector(900.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the foreign (never registered) enemy spawns"), Foreign))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	TestEqual(TEXT("one wave-0 enemy was born before the death"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	AMeleeEnemy* OldBorn = M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]);
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestTrue(TEXT("the run failed from the player death"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestTrue(TEXT("the old run's born enemy is still valid before the retry"), IsValid(OldBorn));

	// The retry: one call, full fresh run.
	if (!TestTrue(TEXT("RetryRoom restarts the failed run"),
		URoomRetryService::RetryRoom(Scene.World, Room, Def, Scene.Player)))
	{
		return true;
	}
	TestFalse(TEXT("the old run's registered enemy was destroyed by the retry"), IsValid(OldBorn));
	TestTrue(TEXT("the foreign (never registered) enemy survived the retry"), IsValid(Foreign));
	TestTrue(TEXT("the retried session is Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestTrue(TEXT("the retry started a new, larger RunId"), Scene.Session->GetRunId() > OldRunId);
	TestTrue(TEXT("the retry started a new, larger SettlementId"),
		Scene.Session->GetSettlementId() > OldSettlementId);
	TestTrue(TEXT("the retry derived a new seed from the new RunId"),
		Scene.Session->GetSeed() != OldSeed);
	TestEqual(TEXT("exactly one run start event per accepted run"), Scene.StartedCount, 2);
	TestEqual(TEXT("exactly one end event so far (the old run's failure)"), Scene.EndedCount, 1);
	TestEqual(TEXT("the new run restarted from wave 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("the new run counts no old kills"), Scene.Session->GetKilledCount(), 0);
	TestEqual(TEXT("the new run counts no old spawns"), Scene.Session->GetSpawnedEnemyCount(), 0);

	// The fresh progression: exactly the configured counts per wave.
	if (!Scene.AdvanceSeconds(*this, 0.35))
	{
		return true;
	}
	TestEqual(TEXT("the new run's wave 0 birthed exactly its two configured enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	TestEqual(TEXT("the world holds the two new enemies plus only the foreign one"),
		M2_010_CountAllEnemies(*Scene.World), 3);
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
			TEXT("the new run's first wave-0 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
			TEXT("the new run's second wave-0 enemy was found")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	TestEqual(TEXT("the new run's wave 1 birthed exactly its three configured enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[0]),
			TEXT("the new run's first wave-1 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[1]),
			TEXT("the new run's second wave-1 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[2]),
			TEXT("the new run's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the retried run clears normally"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the retried run fired its own end event"), Scene.EndedCount, 2);
	if (Scene.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("the retried run's result is Cleared"), Scene.EndedResults[1].bCleared);
		TestEqual(TEXT("the retried run's result carries the new run id"),
			Scene.EndedResults[1].RunId, Scene.Session->GetRunId());
		TestEqual(TEXT("the retried run's result counts exactly its five real deaths"),
			Scene.EndedResults[1].KilledCount, 5);
	}
	return true;
}

// 4. Acceptance: five consecutive retries (six full lifecycles) never
//    accumulate anything: every run's RunId advances by exactly one, every
//    wave holds exactly its configured count, the world never carries a
//    leftover enemy from an earlier run, and the start/end event counters
//    stay exactly one per lifecycle (no delegate stacking anywhere).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_010FiveConsecutiveRetriesKeepWaveCountsExact,
	"UEMMO.Tasks.M2_010.FiveConsecutiveRetriesKeepWaveCountsExact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_010FiveConsecutiveRetriesKeepWaveCountsExact::RunTest(const FString& Parameters)
{
	FM2_010_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_010_MakeRoom();
	UEnemyDefinition* Def = M2_010_MakeEnemyDef();

	if (!TestTrue(TEXT("the first run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 FirstRunId = Scene.Session->GetRunId();
	uint64 PreviousSettlementId = Scene.Session->GetSettlementId();
	if (!TestTrue(TEXT("the first progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}

	// Six lifecycles: the initial run plus five retries. Every lifecycle runs
	// the full wave-0 progression, dies mid wave 1, and (except for the last)
	// is followed by one retry at the start of the next iteration.
	for (int32 Lifecycle = 0; Lifecycle < 6; ++Lifecycle)
	{
		if (Lifecycle > 0)
		{
			if (!TestTrue(FString::Printf(TEXT("retry %d restarted the failed run"), Lifecycle),
				URoomRetryService::RetryRoom(Scene.World, Room, Def, Scene.Player)))
			{
				return true;
			}
			TestEqual(TEXT("the retry advanced the RunId by exactly one"),
				Scene.Session->GetRunId(), FirstRunId + static_cast<uint64>(Lifecycle));
			TestTrue(TEXT("the retry advanced the SettlementId monotonically"),
				Scene.Session->GetSettlementId() > PreviousSettlementId);
			TestEqual(TEXT("exactly one start event per lifecycle (no stacking)"),
				Scene.StartedCount, Lifecycle + 1);
		}
		PreviousSettlementId = Scene.Session->GetSettlementId();

		// Wave 0: exactly two configured enemies, and the world holds ONLY
		// them (every enemy of the earlier run was destroyed by the retry).
		if (!Scene.AdvanceSeconds(*this, 0.35))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("lifecycle %d: wave 0 birthed exactly two enemies"), Lifecycle),
			Scene.Session->GetSpawnedEnemyCount(), 2);
		TestEqual(FString::Printf(TEXT("lifecycle %d: the world holds exactly the two new enemies"), Lifecycle),
			M2_010_CountAllEnemies(*Scene.World), 2);
		TestEqual(FString::Printf(TEXT("lifecycle %d: both new enemies are alive"), Lifecycle),
			M2_010_CountAliveEnemies(*Scene.World), 2);

		if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
				TEXT("the first wave-0 enemy was found"))
			|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
				TEXT("the second wave-0 enemy was found")))
		{
			return true;
		}
		if (!Scene.AdvanceSeconds(*this, 2.0))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("lifecycle %d: wave 1 birthed exactly three enemies"), Lifecycle),
			Scene.Session->GetSpawnedEnemyCount(), 5);
		TestEqual(FString::Printf(TEXT("lifecycle %d: exactly three wave-1 enemies are alive"), Lifecycle),
			M2_010_CountAliveEnemies(*Scene.World), 3);

		// The player dies mid wave 1; the next iteration retries.
		if (!Scene.KillPlayer(*this))
		{
			return true;
		}
		TestTrue(FString::Printf(TEXT("lifecycle %d: the death failed the run"), Lifecycle),
			Scene.Session->GetState() == ERoomSessionState::Failed);
		TestEqual(FString::Printf(TEXT("lifecycle %d: exactly one end event fired (no stacking)"), Lifecycle),
			Scene.EndedCount, Lifecycle + 1);
		TestEqual(FString::Printf(TEXT("lifecycle %d: no unborn spawns remain"), Lifecycle),
			Scene.Session->GetRunPendingSpawnCount(), 0);
	}

	TestEqual(TEXT("six lifecycles produced exactly six start events"), Scene.StartedCount, 6);
	TestEqual(TEXT("six lifecycles produced exactly six end events"), Scene.EndedCount, 6);
	TestEqual(TEXT("six lifecycles produced exactly six death broadcasts"), Scene.PlayerDiedCount, 6);
	TestEqual(TEXT("the final RunId advanced by exactly five retries"),
		Scene.Session->GetRunId(), FirstRunId + 5);
	return true;
}

// 5. Acceptance: old-run events never pollute the new run. A stale kill
//    notification carrying an old-run enemy id is counted (M2-006 counting
//    semantics) but never advances, settles or fails the new run, and a far
//    future clock injection (a leaked old wave wait would fire here) leaves
//    the new run on its own schedule. The new run then completes normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_010OldRunEventsDoNotPolluteNewRun,
	"UEMMO.Tasks.M2_010.OldRunEventsDoNotPolluteNewRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_010OldRunEventsDoNotPolluteNewRun::RunTest(const FString& Parameters)
{
	FM2_010_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_010_MakeRoom();
	UEnemyDefinition* Def = M2_010_MakeEnemyDef();

	if (!TestTrue(TEXT("the old run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	if (!TestTrue(TEXT("the old progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestTrue(TEXT("the old run failed from the player death"),
		Scene.Session->GetState() == ERoomSessionState::Failed);

	if (!TestTrue(TEXT("the retry starts the new run"),
		URoomRetryService::RetryRoom(Scene.World, Room, Def, Scene.Player)))
	{
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	if (!Scene.AdvanceSeconds(*this, 0.35))
	{
		return true;
	}
	TestEqual(TEXT("the new run's wave 0 birthed its two enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);

	// A stale old-run death event arriving during the new run (an id of the
	// old run's wave 1 that the new run's current wave never spawned): the
	// M2-006 counting semantics accept it, the progression must not move.
	TestTrue(TEXT("the stale old-run kill notification is accepted (M2-006 counting)"),
		Scene.Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w1_s2"))));
	TestEqual(TEXT("the stale notification was counted once"), Scene.Session->GetKilledCount(), 1);
	TestTrue(TEXT("the stale notification left the new run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the stale notification started no wave"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("the stale notification fired no end event"), Scene.EndedCount, 1);

	// Far future injections: a leaked old-run wave wait would start wave 1
	// here despite the alive wave-0 enemies. The new run stays on its own
	// schedule instead.
	if (!Scene.AdvanceSeconds(*this, 5.0))
	{
		return true;
	}
	TestEqual(TEXT("the far future injection started no wave-1 while wave-0 enemies live"),
		Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("the far future injection birthed no extra enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	TestTrue(TEXT("the new run is still Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("still exactly one end event (the old run's failure)"), Scene.EndedCount, 1);

	// The new run completes its own 2+3 progression normally: the stale count
	// rides along but never advances a wave.
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
			TEXT("the new run's first wave-0 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
			TEXT("the new run's second wave-0 enemy was found")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[0]),
			TEXT("the new run's first wave-1 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[1]),
			TEXT("the new run's second wave-1 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[2]),
			TEXT("the new run's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the new run cleared despite the stale old-run event"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("exactly two end events (old failure, new clear)"), Scene.EndedCount, 2);
	if (Scene.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("the new run's result is Cleared"), Scene.EndedResults[1].bCleared);
		TestEqual(TEXT("the new run's result carries the new run id"),
			Scene.EndedResults[1].RunId, NewRunId);
		TestEqual(TEXT("the new run's result counts the stale plus its five real deaths"),
			Scene.EndedResults[1].KilledCount, 6);
	}
	return true;
}

// 6. Acceptance: a player death OUTSIDE a Running run is never a failure
//    request: while Idle and while Cleared the broadcast changes nothing -
//    no FailRun, no end event, no state or counter disturbance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_010NonRunningPlayerDeathNeverFailsRun,
	"UEMMO.Tasks.M2_010.NonRunningPlayerDeathNeverFailsRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_010NonRunningPlayerDeathNeverFailsRun::RunTest(const FString& Parameters)
{
	FM2_010_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_010_MakeRoom();
	UEnemyDefinition* Def = M2_010_MakeEnemyDef();

	// Idle: the death happens before any run exists.
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestTrue(TEXT("the death while Idle left the session Idle"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the death while Idle fired no end event"), Scene.EndedCount, 0);

	// Revive (the M2-004 reset opens a fresh death lifecycle) and run a full
	// natural clear; the player stays alive through the whole run.
	Scene.Player->ApplyTrainingRoomReset();
	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room))
		|| !TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	if (!Scene.AdvanceSeconds(*this, 0.35))
	{
		return true;
	}
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]),
			TEXT("the first wave-0 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]),
			TEXT("the second wave-0 enemy was found")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	if (!M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[0]),
			TEXT("the first wave-1 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[1]),
			TEXT("the second wave-1 enemy was found"))
		|| !M2_010_KillEnemy(*this, M2_010_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[2]),
			TEXT("the third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the run cleared naturally"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the clear fired exactly one end event"), Scene.EndedCount, 1);
	const uint64 ClearedRunId = Scene.Session->GetRunId();

	// Cleared: the death broadcast after the terminal state must not fail,
	// re-settle or disturb anything.
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestTrue(TEXT("the death while Cleared left the run Cleared"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the death while Cleared fired no second end event"), Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the recorded outcome is still the clear"), Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the recorded outcome keeps the cleared run id"),
			Scene.EndedResults[0].RunId, ClearedRunId);
	}
	TestEqual(TEXT("the death while Cleared did not reset the spawn bookkeeping"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	return true;
}

#endif
