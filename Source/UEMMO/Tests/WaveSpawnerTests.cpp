// M2-007: cancellable single-wave spawner. UWaveSpawner births the enemies of
// one FRoomWaveDefinition at the configured world-space SpawnLocations, one
// every 0.3 s, driven exclusively by an injected clock through UpdateWave
// (no FTimerHandle anywhere: the tests pass explicit times, so the schedule
// is fully deterministic). The tests reuse the engine FTestWorldWrapper
// precedent from EnemyApproachTests (a real, BeginPlay-initialized temp game
// world with real SpawnActor/BeginPlay) plus the M2-006 precedent of pulling
// the real URoomSessionSubsystem from the world's subsystem collection, so
// the spawner's session integration runs against the production types.
//
// Locked behavior: exactly Count enemies are born and registered (no extras),
// CancelWave stops future births without touching born or foreign actors,
// every refusal/failure comes back as an explicit status (a failed start
// never queues or registers anything), each born enemy is death-bound once
// (its first death leaves AliveIds and counts one session kill; duplicate
// death notifications of the same enemy are idempotent), and PendingSpawns
// stays honest so no one can settle a wave whose enemies were never born.
#include "Misc/AutomationTest.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/WaveSpawner.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_007
{
	// Fixed simulated frame step (60 fps) for the world ticks of the
	// no-friendly-fire suite.
	constexpr float M2_007_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena or
	// other suites' content (per-suite corner convention). X horizontal,
	// Y depth, Z height; the floor top sits exactly at the base Z.
	const FVector M2_007_SceneBase(52000.0, 48000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_007_FloorHalfThickness = 100.0f;
	const float M2_007_FloorHalfExtentXY = 4000.0f;

	// Enemies drop a short distance onto the floor top when the world ticks.
	const float M2_007_EnemySpawnHeight = 90.0f;

	// Card contract: the wave spawns one enemy every 0.3 s (the spawner owns
	// this constant; the tests pin the observable timing against it).
	constexpr double M2_007_SpawnIntervalSeconds = 0.3;

	// Two well-separated world-space spawn locations for the Count=2 wave.
	static TArray<FVector> M2_007_MakeSpawnLocations()
	{
		TArray<FVector> Locations;
		Locations.Add(M2_007_SceneBase + FVector(-300.0, 0.0, M2_007_EnemySpawnHeight));
		Locations.Add(M2_007_SceneBase + FVector(300.0, 0.0, M2_007_EnemySpawnHeight));
		return Locations;
	}

	// Minimal legal room definition carrying exactly one wave (the spawner
	// reads Waves[WaveIndex]; map/bounds/reward data is irrelevant here).
	static URoomDefinition* M2_007_MakeRoom(FName RoomId, int32 Count, const TArray<FVector>& Locations, FName EnemyId)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = RoomId;
		FRoomWaveDefinition Wave;
		Wave.EnemyId = EnemyId;
		Wave.Count = Count;
		Wave.SpawnLocations = Locations;
		Room->Waves.Add(Wave);
		return Room;
	}

	// Definition double of the Data/enemies.json "melee_grunt" row (source
	// JSON is never read at runtime; the spawner takes the definition as a
	// parameter - the JSON to asset loading belongs to a later catalog task).
	static UEnemyDefinition* M2_007_MakeEnemyDef(FName EnemyId)
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

	// World-static blocking floor box so ticked enemies settle on it (temp
	// worlds ship no geometry; same builder pattern as the M1/M2 suites).
	static AActor* M2_007_SpawnFloor(UWorld& World)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), M2_007_SceneBase - FVector(0.0, 0.0, M2_007_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M2_007_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M2_007_FloorHalfExtentXY, M2_007_FloorHalfExtentXY, M2_007_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(M2_007_SceneBase - FVector(0.0, 0.0, M2_007_FloorHalfThickness));
		return Actor;
	}

	// Spawns a melee enemy OUTSIDE the spawner (foreign actor for the
	// no-friendly-fire suite; the constructor already enables no-controller
	// physics, restated defensively like the M2-002 precedent).
	static AMeleeEnemy* M2_007_SpawnForeignEnemy(UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		AMeleeEnemy* Enemy = World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Enemy != nullptr && Enemy->GetCharacterMovement() != nullptr)
		{
			Enemy->GetCharacterMovement()->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// Number of AMeleeEnemy actors alive in the world (spawner-born or not).
	static int32 M2_007_CountMeleeEnemies(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	// First melee enemy standing at the given world-space location (the
	// spawner births at the exact configured location, AlwaysSpawn policy).
	static AMeleeEnemy* M2_007_FindEnemyAt(UWorld& World, const FVector& Location)
	{
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			if (FVector::DistSquared(It->GetActorLocation(), Location) < 1.0)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// One temp world with its real session subsystem, a floor and one rooted
	// spawner per test. The spawner is rooted so the world ticks of the
	// no-friendly-fire suite can never garbage collect it mid-test.
	struct FM2_007_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		UWaveSpawner* Spawner = nullptr;

		virtual ~FM2_007_Scene()
		{
			if (Spawner != nullptr)
			{
				Spawner->RemoveFromRoot();
				Spawner = nullptr;
			}
		}

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
			AActor* Floor = M2_007_SpawnFloor(*World);
			if (!Test.TestNotNull(TEXT("the test floor spawns"), Floor))
			{
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();

			Spawner = NewObject<UWaveSpawner>(GetTransientPackage(), NAME_None, RF_Transient);
			if (!Test.TestNotNull(TEXT("the wave spawner is created"), Spawner))
			{
				return false;
			}
			Spawner->AddToRoot();
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M2_007;

// 1. Acceptance: requesting Count=2 ends with exactly two registered enemies
//    (AliveIds=2, PendingSpawns=0), born one 0.3 s interval apart at the two
//    configured SpawnLocations, with the session bookkeeping (wave index,
//    spawned count) in lockstep - and nothing born before the first
//    UpdateWave, so no one can settle a wave whose enemies never existed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_007WaveSpawnsExactlyRequestedCountOverInterval,
	"UEMMO.Tasks.M2_007.WaveSpawnsExactlyRequestedCountOverInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_007WaveSpawnsExactlyRequestedCountOverInterval::RunTest(const FString& Parameters)
{
	FM2_007_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	const TArray<FVector> Locations = M2_007_MakeSpawnLocations();
	URoomDefinition* Room = M2_007_MakeRoom(FName(TEXT("room_wave_test")), 2, Locations, FName(TEXT("melee_grunt")));
	UEnemyDefinition* Def = M2_007_MakeEnemyDef(FName(TEXT("melee_grunt")));
	if (!TestTrue(TEXT("StartRoom accepts the run the wave belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}

	TestTrue(TEXT("a fresh spawner is Idle"), Scene.Spawner->GetState() == EWaveState::Idle);
	Scene.Spawner->UpdateWave(1000.0);
	TestTrue(TEXT("UpdateWave before StartWave is a no-op"), Scene.Spawner->GetState() == EWaveState::Idle);

	TestTrue(TEXT("StartWave accepts a legal request while the session is Running"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::Started);
	TestEqual(TEXT("the wave index is recorded in the session"), Scene.Session->GetCurrentWaveIndex(), 0);

	// Nothing may exist before the first UpdateWave (no premature settlement).
	TestTrue(TEXT("the wave is Spawning before the first UpdateWave"), Scene.Spawner->GetState() == EWaveState::Spawning);
	TestEqual(TEXT("both enemies are still pending before the first UpdateWave"), Scene.Spawner->GetPendingSpawnCount(), 2);
	TestEqual(TEXT("no enemy is alive before the first UpdateWave"), Scene.Spawner->GetAliveEnemyIds().Num(), 0);
	TestEqual(TEXT("no enemy is registered in the session before birth"), Scene.Session->GetSpawnedEnemyCount(), 0);
	TestEqual(TEXT("no melee enemy exists in the world before the first UpdateWave"), M2_007_CountMeleeEnemies(*Scene.World), 0);

	// First UpdateWave anchors the wave start and births slot 0.
	Scene.Spawner->UpdateWave(0.0);
	TestEqual(TEXT("the first UpdateWave leaves one enemy pending"), Scene.Spawner->GetPendingSpawnCount(), 1);
	TestEqual(TEXT("the first UpdateWave tracks exactly one alive id"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("the session registered exactly one spawn"), Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("exactly one melee enemy exists in the world"), M2_007_CountMeleeEnemies(*Scene.World), 1);
	AMeleeEnemy* First = M2_007_FindEnemyAt(*Scene.World, Locations[0]);
	TestNotNull(TEXT("the first enemy was born at the first configured SpawnLocation"), First);
	if (First != nullptr)
	{
		TestTrue(TEXT("the first enemy carries the handed enemy definition"), First->GetEnemyDefinition() == Def);
		TestTrue(TEXT("the first enemy is alive"),
			First->GetHealthComponent() != nullptr && First->GetHealthComponent()->IsAlive());
	}

	// SpawnInterval: half the 0.3 s interval after the anchor, slot 1 is not due.
	Scene.Spawner->UpdateWave(M2_007_SpawnIntervalSeconds * 0.5);
	TestEqual(TEXT("the second enemy is still pending before the interval elapsed"), Scene.Spawner->GetPendingSpawnCount(), 1);
	TestEqual(TEXT("the second enemy was not born early"), M2_007_CountMeleeEnemies(*Scene.World), 1);

	// At the 0.3 s mark slot 1 births and the spawn work of the wave is done.
	Scene.Spawner->UpdateWave(M2_007_SpawnIntervalSeconds);
	TestEqual(TEXT("the second enemy was born at the 0.3 s interval (pending)"), Scene.Spawner->GetPendingSpawnCount(), 0);
	TestEqual(TEXT("both alive ids are tracked after the interval"), Scene.Spawner->GetAliveEnemyIds().Num(), 2);
	TestEqual(TEXT("the session registered both spawns"), Scene.Session->GetSpawnedEnemyCount(), 2);
	TestTrue(TEXT("the wave is Complete once every requested enemy was born"),
		Scene.Spawner->GetState() == EWaveState::Complete);
	TestEqual(TEXT("exactly two melee enemies exist in the world"), M2_007_CountMeleeEnemies(*Scene.World), 2);
	AMeleeEnemy* Second = M2_007_FindEnemyAt(*Scene.World, Locations[1]);
	TestNotNull(TEXT("the second enemy was born at the second configured SpawnLocation"), Second);
	TestTrue(TEXT("the two tracked alive ids are distinct"),
		Scene.Spawner->GetAliveEnemyIds().Num() == 2
		&& Scene.Spawner->GetAliveEnemyIds()[0] != Scene.Spawner->GetAliveEnemyIds()[1]);

	// Fully overdue drives never birth extras beyond the requested count.
	Scene.Spawner->UpdateWave(M2_007_SpawnIntervalSeconds * 2.0);
	Scene.Spawner->UpdateWave(100.0);
	TestEqual(TEXT("no extra alive ids appear after the wave completed"), Scene.Spawner->GetAliveEnemyIds().Num(), 2);
	TestEqual(TEXT("the world still holds exactly two melee enemies"), M2_007_CountMeleeEnemies(*Scene.World), 2);

	// The single-wave spawner refuses a second StartWave.
	TestTrue(TEXT("a second StartWave is refused by the single-wave spawner"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::RejectedAlreadyStarted);
	return true;
}

// 2. Acceptance: cancelling after the first birth stops all future births -
//    the second enemy of the Count=2 wave is never born (PendingSpawns=0,
//    AliveIds stays 1) no matter how far the injected clock is driven.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_007CancelAfterFirstSpawnStopsFutureBirths,
	"UEMMO.Tasks.M2_007.CancelAfterFirstSpawnStopsFutureBirths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_007CancelAfterFirstSpawnStopsFutureBirths::RunTest(const FString& Parameters)
{
	FM2_007_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	const TArray<FVector> Locations = M2_007_MakeSpawnLocations();
	URoomDefinition* Room = M2_007_MakeRoom(FName(TEXT("room_wave_test")), 2, Locations, FName(TEXT("melee_grunt")));
	UEnemyDefinition* Def = M2_007_MakeEnemyDef(FName(TEXT("melee_grunt")));

	// Cancelling a spawner that never started is a harmless no-op.
	Scene.Spawner->CancelWave();
	TestTrue(TEXT("CancelWave on a fresh spawner leaves it Idle"), Scene.Spawner->GetState() == EWaveState::Idle);

	if (!TestTrue(TEXT("StartRoom accepts the run the wave belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("StartWave accepts the legal request"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::Started);

	// One enemy born, one still pending.
	Scene.Spawner->UpdateWave(0.0);
	TestEqual(TEXT("the first enemy was born (one alive id)"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("the second enemy is still pending before the cancel"), Scene.Spawner->GetPendingSpawnCount(), 1);

	Scene.Spawner->CancelWave();
	TestEqual(TEXT("the cancel zeroes the pending spawns"), Scene.Spawner->GetPendingSpawnCount(), 0);
	TestTrue(TEXT("the wave is Cancelled"), Scene.Spawner->GetState() == EWaveState::Cancelled);

	// Driving far past every due time must never produce the second enemy.
	Scene.Spawner->UpdateWave(M2_007_SpawnIntervalSeconds);
	Scene.Spawner->UpdateWave(2.0);
	Scene.Spawner->UpdateWave(1000.0);
	TestEqual(TEXT("the second enemy was never born (alive ids)"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("the pending count stays zero after the cancel"), Scene.Spawner->GetPendingSpawnCount(), 0);
	TestEqual(TEXT("the world holds exactly the one born enemy"), M2_007_CountMeleeEnemies(*Scene.World), 1);

	AMeleeEnemy* First = M2_007_FindEnemyAt(*Scene.World, Locations[0]);
	TestTrue(TEXT("the born enemy is still a valid actor after the cancel"), IsValid(First));
	if (First != nullptr)
	{
		TestTrue(TEXT("the born enemy is still alive after the cancel"),
			First->GetHealthComponent() != nullptr && First->GetHealthComponent()->IsAlive());
	}
	return true;
}

// 3. Acceptance: a generation failure never pretends the wave passed - every
//    invalid request (no session, missing definitions, out-of-range index,
//    illegal wave data, idle session) returns its own explicit failure
//    status, leaves PendingSpawns at zero and registers nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_007StartWaveFailuresReportExplicitStatus,
	"UEMMO.Tasks.M2_007.StartWaveFailuresReportExplicitStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_007StartWaveFailuresReportExplicitStatus::RunTest(const FString& Parameters)
{
	FM2_007_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	const TArray<FVector> Locations = M2_007_MakeSpawnLocations();
	URoomDefinition* Room = M2_007_MakeRoom(FName(TEXT("room_wave_test")), 2, Locations, FName(TEXT("melee_grunt")));
	UEnemyDefinition* Def = M2_007_MakeEnemyDef(FName(TEXT("melee_grunt")));
	// Illegal wave data: Count=2 but only one spawn location.
	TArray<FVector> ShortLocations;
	ShortLocations.Add(Locations[0]);
	URoomDefinition* ShortRoom = M2_007_MakeRoom(FName(TEXT("room_short_wave")), 2, ShortLocations, FName(TEXT("melee_grunt")));
	// Mismatched wave data: the wave names another enemy kind than the
	// definition that was handed over (legal Count=1 data, wrong EnemyId).
	URoomDefinition* OtherRoom = M2_007_MakeRoom(FName(TEXT("room_other_wave")), 1, ShortLocations, FName(TEXT("other_grunt")));

	// A fresh session is Idle: the wave is refused until a run is accepted.
	TestTrue(TEXT("StartWave is refused while the session is not Running"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::RejectedSessionNotRunning);
	TestTrue(TEXT("the refused start leaves the spawner Idle"), Scene.Spawner->GetState() == EWaveState::Idle);
	TestEqual(TEXT("the refused start queues no pending spawns"), Scene.Spawner->GetPendingSpawnCount(), 0);

	if (!TestTrue(TEXT("StartRoom accepts the run the wave belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}

	// No session handed over: no world to spawn into, no run to register with.
	TestTrue(TEXT("StartWave without a session is refused"),
		Scene.Spawner->StartWave(Room, 0, Def, nullptr) == EWaveStartResult::RejectedNoSession);
	// Missing definitions.
	TestTrue(TEXT("StartWave with a null enemy definition is refused"),
		Scene.Spawner->StartWave(Room, 0, nullptr, Scene.Session) == EWaveStartResult::RejectedInvalidRequest);
	TestTrue(TEXT("StartWave with a null room definition is refused"),
		Scene.Spawner->StartWave(nullptr, 0, Def, Scene.Session) == EWaveStartResult::RejectedInvalidRequest);
	// Out-of-range wave index.
	TestTrue(TEXT("StartWave with an out-of-range wave index is refused"),
		Scene.Spawner->StartWave(Room, 7, Def, Scene.Session) == EWaveStartResult::RejectedInvalidRequest);
	// Illegal wave: Count does not match the spawn locations.
	TestTrue(TEXT("StartWave with a Count/location mismatch is refused"),
		Scene.Spawner->StartWave(ShortRoom, 0, Def, Scene.Session) == EWaveStartResult::RejectedInvalidRequest);
	// Illegal wave: the handed definition is not the wave's enemy kind.
	TestTrue(TEXT("StartWave with a mismatched enemy definition is refused"),
		Scene.Spawner->StartWave(OtherRoom, 0, Def, Scene.Session) == EWaveStartResult::RejectedInvalidRequest);

	// Every refusal stayed bookkeeping-clean: nothing queued, registered, born.
	TestTrue(TEXT("the refused starts leave the spawner Idle"), Scene.Spawner->GetState() == EWaveState::Idle);
	TestEqual(TEXT("the refused starts queue no pending spawns"), Scene.Spawner->GetPendingSpawnCount(), 0);
	TestEqual(TEXT("the refused starts track no alive ids"), Scene.Spawner->GetAliveEnemyIds().Num(), 0);
	TestEqual(TEXT("the refused starts register no session spawns"), Scene.Session->GetSpawnedEnemyCount(), 0);
	TestEqual(TEXT("the refused starts count no kills"), Scene.Session->GetKilledCount(), 0);
	TestEqual(TEXT("the refused starts leave no enemy in the world"), M2_007_CountMeleeEnemies(*Scene.World), 0);

	// Failures do not poison the spawner: a legal start is still accepted
	// afterwards (and cancelled immediately to leave the scene clean).
	TestTrue(TEXT("a legal StartWave is still accepted after the refusals"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::Started);
	Scene.Spawner->CancelWave();
	TestEqual(TEXT("the cleanup cancel registered no spawns"), Scene.Session->GetSpawnedEnemyCount(), 0);
	return true;
}

// 4. Acceptance: each born enemy is bound to die exactly once - the first
//    death removes its alive id and counts one session kill; a duplicate
//    death notification of the same enemy (second lifecycle after a reset)
//    is idempotent and never double-counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_007EnemyDeathUpdatesAliveIdsAndSessionKillsIdempotently,
	"UEMMO.Tasks.M2_007.EnemyDeathUpdatesAliveIdsAndSessionKillsIdempotently",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_007EnemyDeathUpdatesAliveIdsAndSessionKillsIdempotently::RunTest(const FString& Parameters)
{
	FM2_007_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	const TArray<FVector> Locations = M2_007_MakeSpawnLocations();
	URoomDefinition* Room = M2_007_MakeRoom(FName(TEXT("room_wave_test")), 2, Locations, FName(TEXT("melee_grunt")));
	UEnemyDefinition* Def = M2_007_MakeEnemyDef(FName(TEXT("melee_grunt")));
	if (!TestTrue(TEXT("StartRoom accepts the run the wave belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("StartWave accepts the legal request"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::Started);
	Scene.Spawner->UpdateWave(0.0);
	Scene.Spawner->UpdateWave(M2_007_SpawnIntervalSeconds);
	TestEqual(TEXT("both enemies are alive before the deaths"), Scene.Spawner->GetAliveEnemyIds().Num(), 2);
	TestEqual(TEXT("no kill is counted before the deaths"), Scene.Session->GetKilledCount(), 0);

	AMeleeEnemy* EnemyA = M2_007_FindEnemyAt(*Scene.World, Locations[0]);
	AMeleeEnemy* EnemyB = M2_007_FindEnemyAt(*Scene.World, Locations[1]);
	TestNotNull(TEXT("the first born enemy is found in the world"), EnemyA);
	TestNotNull(TEXT("the second born enemy is found in the world"), EnemyB);
	if (EnemyA == nullptr || EnemyB == nullptr)
	{
		return true;
	}

	// First death: the alive id leaves and the session counts exactly one kill.
	EnemyA->GetHealthComponent()->ApplyDamage(999.0f);
	TestTrue(TEXT("the first enemy died from the damage"), !EnemyA->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the dead enemy left the alive ids"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("the session counted the first kill"), Scene.Session->GetKilledCount(), 1);

	// Duplicate death notification of the SAME enemy (ResetHealth opens a
	// second death lifecycle, OnDied fires again): idempotent, never counted.
	EnemyA->GetHealthComponent()->ResetHealth();
	EnemyA->GetHealthComponent()->ApplyDamage(999.0f);
	TestEqual(TEXT("a duplicate death of the same enemy keeps the alive ids"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("a duplicate death of the same enemy keeps the session kills"), Scene.Session->GetKilledCount(), 1);

	// The second enemy dies: the wave is fully dead and exactly two kills exist.
	EnemyB->GetHealthComponent()->ApplyDamage(999.0f);
	TestTrue(TEXT("the second enemy died from the damage"), !EnemyB->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the second death empties the alive ids"), Scene.Spawner->GetAliveEnemyIds().Num(), 0);
	TestEqual(TEXT("the session counted exactly the two real deaths"), Scene.Session->GetKilledCount(), 2);
	return true;
}

// 5. Acceptance: CancelWave does no friendly fire - the enemy born before the
//    cancel stays valid and alive through continued world ticking, and a
//    foreign melee enemy spawned outside the spawner is never touched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_007CancelDoesNotHarmSpawnedOrForeignActors,
	"UEMMO.Tasks.M2_007.CancelDoesNotHarmSpawnedOrForeignActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_007CancelDoesNotHarmSpawnedOrForeignActors::RunTest(const FString& Parameters)
{
	FM2_007_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	const TArray<FVector> Locations = M2_007_MakeSpawnLocations();
	URoomDefinition* Room = M2_007_MakeRoom(FName(TEXT("room_wave_test")), 2, Locations, FName(TEXT("melee_grunt")));
	UEnemyDefinition* Def = M2_007_MakeEnemyDef(FName(TEXT("melee_grunt")));

	// Foreign actor spawned OUTSIDE the spawner, before any wave existed.
	AMeleeEnemy* Bystander = M2_007_SpawnForeignEnemy(*Scene.World,
		M2_007_SceneBase + FVector(0.0, -600.0, M2_007_EnemySpawnHeight));
	if (!TestNotNull(TEXT("the foreign bystander enemy spawns outside the spawner"), Bystander))
	{
		return true;
	}

	if (!TestTrue(TEXT("StartRoom accepts the run the wave belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("StartWave accepts the legal request"),
		Scene.Spawner->StartWave(Room, 0, Def, Scene.Session) == EWaveStartResult::Started);
	Scene.Spawner->UpdateWave(0.0);
	TestEqual(TEXT("the first enemy was born before the cancel"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);

	Scene.Spawner->CancelWave();
	Scene.Spawner->UpdateWave(M2_007_SpawnIntervalSeconds);
	Scene.Spawner->UpdateWave(10.0);
	TestEqual(TEXT("the cancel stopped the second birth (alive ids)"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("the world holds exactly the bystander and the born enemy"), M2_007_CountMeleeEnemies(*Scene.World), 2);

	AMeleeEnemy* Spawned = M2_007_FindEnemyAt(*Scene.World, Locations[0]);
	TestNotNull(TEXT("the spawner's born enemy is found"), Spawned);
	// Ticking the world must not make the cancel retroactive: no born or
	// foreign actor may be destroyed or killed by the cancel path.
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M2_007_FrameSeconds)))
		{
			return true;
		}
	}
	TestTrue(TEXT("the born enemy is still a valid actor after the ticking"), IsValid(Spawned));
	if (Spawned != nullptr)
	{
		TestTrue(TEXT("the born enemy is still alive after the ticking"),
			Spawned->GetHealthComponent() != nullptr && Spawned->GetHealthComponent()->IsAlive());
	}
	TestTrue(TEXT("the foreign bystander is still a valid actor after the ticking"), IsValid(Bystander));
	if (Bystander != nullptr)
	{
		TestTrue(TEXT("the foreign bystander is still alive after the ticking"),
			Bystander->GetHealthComponent() != nullptr && Bystander->GetHealthComponent()->IsAlive());
	}
	TestEqual(TEXT("the alive ids still track exactly the born enemy"), Scene.Spawner->GetAliveEnemyIds().Num(), 1);
	TestEqual(TEXT("the world still holds exactly two melee enemies"), M2_007_CountMeleeEnemies(*Scene.World), 2);
	return true;
}

#endif
