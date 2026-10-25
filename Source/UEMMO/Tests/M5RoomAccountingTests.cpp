// M5-067: idempotent room birth/death accounting at the PUBLIC session
// entries. NotifyEnemySpawned accepts only a NEW non-empty instance id of the
// running run (empty, duplicate and re-registered ids are refused with no
// side effects), and NotifyEnemyKilled accepts only an id currently in this
// run's alive set (unknown, duplicate, definition-level and old-run ids are
// refused without touching KilledCount, the alive set or the wave
// progression). The production WaveSpawner path keeps its own filters; these
// tests prove the session entries stay independently safe when called
// directly, and that the normal 2+3 wave settlement still clears through the
// hardened entries.
//
// The suite reuses the M2-006 bare-world precedent for the pure bookkeeping
// tests and the M2-008 FTestWorldWrapper precedent (a real, BeginPlay-
// initialized temp game world with the production subsystem and real
// AMeleeEnemy actors) for the settlement test. Deaths in the settlement test
// go through the enemies' health components, i.e. the exact production chain
// (HealthComponent OnDied -> WaveSpawner::HandleEnemyDied ->
// NotifyEnemyKilled).

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#include "EngineUtils.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_067
{
	// One fresh bare game world per test (the M2-006 precedent): enough for
	// the pure accounting APIs, which need no actors and no BeginPlay.
	static UWorld* M5_067_AcquireTestWorld()
	{
		static int32 M5_067_WorldCounter = 0;
		++M5_067_WorldCounter;
		const FName WorldName = FName(*FString::Printf(TEXT("M5_067_TestWorld_%d"), M5_067_WorldCounter));
		return UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
	}

	static URoomSessionSubsystem* M5_067_GetSession(FAutomationTestBase& Test, UWorld* World)
	{
		if (!Test.TestNotNull(TEXT("a test world is available"), World))
		{
			return nullptr;
		}
		URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
		if (!Test.TestNotNull(TEXT("the room session subsystem exists in the world's subsystem collection"), Session))
		{
			return nullptr;
		}
		return Session;
	}

	// Minimal room definition: the accounting entries only read the room id.
	static URoomDefinition* M5_067_MakeRoom(FName RoomId)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = RoomId;
		return Room;
	}

	// Enemy definition matching the M2-008 suite values (the settlement test
	// spawns real actors from it).
	static UEnemyDefinition* M5_067_MakeEnemyDef(FName EnemyId)
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = EnemyId;
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

	// Hand-built legal 2+3 room (the room_training_01 shape, in code, so the
	// suite does not depend on the JSON catalog).
	static URoomDefinition* M5_067_MakeTwoThreeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m5_067_accounting"));
		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 2;
		Wave0.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
		Wave0.SpawnLocations.Add(FVector(600.0, 0.0, 88.0));
		Room->Waves.Add(Wave0);
		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = FName(TEXT("melee_grunt"));
		Wave1.Count = 3;
		Wave1.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
		Wave1.SpawnLocations.Add(FVector(600.0, 0.0, 88.0));
		Wave1.SpawnLocations.Add(FVector(900.0, 0.0, 88.0));
		Room->Waves.Add(Wave1);
		return Room;
	}

	// Recorded session events (the M2-006/M2-008 precedent).
	struct FM5_067_RunEvents
	{
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;

		void Bind(URoomSessionSubsystem& Session)
		{
			Session.OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
			});
			Session.OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});
		}
	};

	// A real BeginPlay-initialized temp game world with its production
	// subsystem (the M2-008 FTestWorldWrapper precedent).
	struct FM5_067_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			return true;
		}
	};

	// First ALIVE melee enemy standing at the given location (the alive-only
	// filter keeps a corpse of an earlier wave from shadowing the fresh enemy
	// born at the same configured location - the M2-008 precedent).
	static AMeleeEnemy* M5_067_FindAliveEnemyAt(UWorld& World, const FVector& Location)
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

	// Kills one enemy through its health component (the production death
	// chain) and verifies the death took.
	static bool M5_067_KillEnemy(FAutomationTestBase& Test, AMeleeEnemy* Enemy, const TCHAR* What)
	{
		if (!Test.TestNotNull(What, Enemy))
		{
			return false;
		}
		Enemy->GetHealthComponent()->ApplyDamage(999.0f);
		return Test.TestTrue(FString::Printf(TEXT("%s - the damage killed it"), What),
			!Enemy->GetHealthComponent()->IsAlive());
	}
}

using namespace UE::UEMMO::Tasks::M5_067;

// 1. Acceptance: the spawn entry accepts only NEW non-empty instance ids of
//    the running run - an empty id, a duplicate registration (alive) and a
//    re-registration of an already-dead id are all refused with no side
//    effects on the spawn count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_067SpawnEntryRejectsEmptyDuplicateAndReregisteredIds,
	"UEMMO.Tasks.M5_067.SpawnEntryRejectsEmptyDuplicateAndReregisteredIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_067SpawnEntryRejectsEmptyDuplicateAndReregisteredIds::RunTest(const FString& Parameters)
{
	UWorld* World = M5_067_AcquireTestWorld();
	URoomSessionSubsystem* Session = M5_067_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	// Outside Running nothing registers.
	TestFalse(TEXT("a spawn notification while Idle is refused"),
		Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the Idle refusal counted no spawn"), Session->GetSpawnedEnemyCount(), 0);

	const URoomDefinition* Room = M5_067_MakeRoom(FName(TEXT("room_m5_067_accounting")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}

	// Empty id: no instance identity at all.
	TestFalse(TEXT("an empty spawn id is refused"), Session->NotifyEnemySpawned(NAME_None));
	TestEqual(TEXT("the empty-id refusal counted no spawn"), Session->GetSpawnedEnemyCount(), 0);

	// A new non-empty instance id registers exactly once.
	TestTrue(TEXT("the first instance id registers"), Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the registration counted one spawn"), Session->GetSpawnedEnemyCount(), 1);

	// Duplicate registration while the id is still alive.
	TestFalse(TEXT("a duplicate registration of the live id is refused"),
		Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the duplicate refusal did not change the spawn count"), Session->GetSpawnedEnemyCount(), 1);

	// Re-registration of an already-dead id: not "new" for this run either -
	// one instance id is registered at most once per run.
	TestTrue(TEXT("the registered id's death is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	TestFalse(TEXT("a re-registration of the dead id is refused"),
		Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the re-registration refusal kept the spawn count"), Session->GetSpawnedEnemyCount(), 1);

	// A distinct instance id still registers after the refusals.
	TestTrue(TEXT("a distinct instance id registers"), Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s1"))));
	TestEqual(TEXT("the distinct registration counted"), Session->GetSpawnedEnemyCount(), 2);
	return true;
}

// 2. Acceptance: the kill entry accepts only ids currently in this run's
//    alive set. Zero births, unknown ids, the definition-level id, duplicate
//    deaths, post-terminal stale callbacks and old-run callbacks during a
//    later run are all refused with no side effects; the SAME instance id is
//    new again in the NEXT run (the retry precedent).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_067KillEntryRejectsUnknownDuplicateOldRunAndDefinitionIds,
	"UEMMO.Tasks.M5_067.KillEntryRejectsUnknownDuplicateOldRunAndDefinitionIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_067KillEntryRejectsUnknownDuplicateOldRunAndDefinitionIds::RunTest(const FString& Parameters)
{
	UWorld* World = M5_067_AcquireTestWorld();
	URoomSessionSubsystem* Session = M5_067_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	const URoomDefinition* Room = M5_067_MakeRoom(FName(TEXT("room_m5_067_accounting")));
	if (!TestTrue(TEXT("run 1 starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 FirstRunId = Session->GetRunId();

	// Zero births: any death is unknown.
	TestFalse(TEXT("a kill notification with zero births is refused"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the zero-birth refusal counted no kill"), Session->GetKilledCount(), 0);
	TestTrue(TEXT("the refusal left the run Running"),
		Session->GetState() == ERoomSessionState::Running);

	TestTrue(TEXT("instance A registers"), Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestTrue(TEXT("instance B registers"), Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s1"))));

	// The enemy DEFINITION id is not an instance id.
	TestFalse(TEXT("the definition-level id is refused as a kill"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestEqual(TEXT("the definition-id refusal counted no kill"), Session->GetKilledCount(), 0);

	TestTrue(TEXT("the death of instance A is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the accepted death counted once"), Session->GetKilledCount(), 1);

	// Duplicate death of the same instance.
	TestFalse(TEXT("a duplicate death of instance A is refused"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	// Unknown id that was never registered.
	TestFalse(TEXT("a death of a never-registered id is refused"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w7_s9"))));
	TestEqual(TEXT("both refusals kept the kill count"), Session->GetKilledCount(), 1);

	TestTrue(TEXT("the death of instance B is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s1"))));
	TestEqual(TEXT("run 1 counts two kills"), Session->GetKilledCount(), 2);

	// After the terminal state the stale callback of run 1 keeps being refused.
	if (!TestTrue(TEXT("run 1 fails"), Session->FailRun()))
	{
		return true;
	}
	TestFalse(TEXT("a stale kill after the terminal state is refused"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the stale refusal kept run 1's kill count"), Session->GetKilledCount(), 2);

	if (!TestTrue(TEXT("the session resets to Idle"), Session->ResetToIdle()))
	{
		return true;
	}

	// Run 2: an old-run callback (run 1's id) is refused because the id is not
	// in run 2's alive set - while the SAME instance id is new again for run 2
	// and may register and die there (the retry flow reuses wave/slot ids).
	if (!TestTrue(TEXT("run 2 starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("run 2 got a fresh run id"), Session->GetRunId() != FirstRunId);
	TestFalse(TEXT("the old-run id's death is refused during run 2"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the old-run refusal counted nothing for run 2"), Session->GetKilledCount(), 0);
	TestTrue(TEXT("the same instance id registers fresh in run 2"),
		Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestTrue(TEXT("and its death counts in run 2"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("run 2 counts its own single kill"), Session->GetKilledCount(), 1);
	return true;
}

// 3. Acceptance: the normal 2+3 wave settlement still works through the
//    hardened entries - five real births (WaveSpawner -> NotifyEnemySpawned),
//    five real health-component deaths (OnDied -> HandleEnemyDied ->
//    NotifyEnemyKilled), the 1.0 s inter-wave wait, and exactly one Cleared
//    settlement carrying KilledCount=5.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_067TwoPlusThreeWavesSettleThroughTheHardenedEntries,
	"UEMMO.Tasks.M5_067.TwoPlusThreeWavesSettleThroughTheHardenedEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_067TwoPlusThreeWavesSettleThroughTheHardenedEntries::RunTest(const FString& Parameters)
{
	FM5_067_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M5_067_MakeTwoThreeRoom();
	UEnemyDefinition* Def = M5_067_MakeEnemyDef(FName(TEXT("melee_grunt")));

	FM5_067_RunEvents Events;
	Events.Bind(*Scene.Session);
	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the waves begin"), Scene.Session->BeginWaves(Room, Def));

	// Wave 0 births (anchor 0.0, slots due at +0.0/+0.3).
	Scene.Session->SetSessionClockSeconds(0.0);
	Scene.Session->SetSessionClockSeconds(0.3);
	TestEqual(TEXT("wave 0 birthed its two enemies through the spawn entry"),
		Scene.Session->GetSpawnedEnemyCount(), 2);

	// A hostile duplicate registration of a live wave-0 id is refused without
	// disturbing the settlement path.
	TestFalse(TEXT("a duplicate spawn of the live wave-0 id is refused"),
		Scene.Session->NotifyEnemySpawned(FName(TEXT("melee_grunt_w0_s0"))));
	TestEqual(TEXT("the refusal kept the spawn count at 2"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// Both wave-0 deaths through the real health chain (last death at 0.3 ->
	// next wave due at 1.3).
	AMeleeEnemy* EnemyA = M5_067_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[0]);
	if (!M5_067_KillEnemy(*this, EnemyA, TEXT("the first wave-0 enemy was found")))
	{
		return true;
	}
	AMeleeEnemy* EnemyB = M5_067_FindAliveEnemyAt(*Scene.World, Room->Waves[0].SpawnLocations[1]);
	if (!M5_067_KillEnemy(*this, EnemyB, TEXT("the second wave-0 enemy was found")))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 deaths counted"), Scene.Session->GetKilledCount(), 2);

	// Wave 1 starts at 1.3 and births its three enemies by 1.9.
	Scene.Session->SetSessionClockSeconds(1.3);
	Scene.Session->SetSessionClockSeconds(1.6);
	Scene.Session->SetSessionClockSeconds(1.9);
	TestEqual(TEXT("wave 1 birthed its three enemies through the spawn entry"),
		Scene.Session->GetSpawnedEnemyCount(), 5);

	AMeleeEnemy* EnemyC = M5_067_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[0]);
	if (!M5_067_KillEnemy(*this, EnemyC, TEXT("the first wave-1 enemy was found")))
	{
		return true;
	}
	AMeleeEnemy* EnemyD = M5_067_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[1]);
	if (!M5_067_KillEnemy(*this, EnemyD, TEXT("the second wave-1 enemy was found")))
	{
		return true;
	}
	AMeleeEnemy* EnemyE = M5_067_FindAliveEnemyAt(*Scene.World, Room->Waves[1].SpawnLocations[2]);
	if (!M5_067_KillEnemy(*this, EnemyE, TEXT("the third wave-1 enemy was found")))
	{
		return true;
	}

	// The last real death settles the run exactly once through the public
	// accounting chain.
	TestTrue(TEXT("the run cleared through the hardened entries"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("all five deaths counted"), Scene.Session->GetKilledCount(), 5);
	TestEqual(TEXT("the settlement fired exactly one end event"), Events.EndedCount, 1);
	if (TestTrue(TEXT("the end event carried one result"), Events.EndedResults.Num() == 1))
	{
		TestTrue(TEXT("the result records Cleared"), Events.EndedResults[0].bCleared);
		TestEqual(TEXT("the result counts all five kills"), Events.EndedResults[0].KilledCount, 5);
	}
	return true;
}

#endif
