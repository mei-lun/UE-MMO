// M5-068: room entry failure handling on the production trigger. The entry
// validates its configuration BEFORE StartRoom - a room definition that does
// not resolve, a definition without waves, or an enemy definition asset that
// does not load refuses the entry (the session stays Idle, the trigger stays
// re-fireable, no default enemy is fabricated), and a restored valid source
// is picked up by the next player entry. When an accepted run then has its
// BeginWaves refused (e.g. an illegal first wave), the trigger ends the run
// through FailRun: the session ends Failed - never a waveless Running state -
// with no kills, no births, no reward eligibility and exactly one end event.
//
// The suite reuses the M2-009 FTestWorldWrapper precedent: a real,
// BeginPlay-initialized temp game world with the production subsystem, real
// overlap dispatch through world ticks, and the real ARoomTrigger configured
// through its M5-068 test/level-scripting setters.

#include "Misc/AutomationTest.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UObjectGlobals.h"

#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/RoomTrigger.h"

#include "Engine/World.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_068
{
	// Fixed test geometry (the M2-009 precedent): the trigger covers
	// x in [-250, 50] (actor at (-100, 0, 88)); the player starts far outside.
	constexpr float M5_068TriggerLocationX = -100.0f;
	constexpr float M5_068OutsideLocationX = -800.0f;
	constexpr float M5_068PlayerZ = 100.0f;

	// The production enemy asset (the trigger's class default source) - the
	// valid configuration every recovery step restores to.
	const TCHAR* M5_068ValidEnemyAssetPath =
		TEXT("/Game/UEMMO/Enemy/Definitions/DA_melee_grunt.DA_melee_grunt");

	// A deliberately missing room definition asset.
	const TCHAR* M5_068MissingRoomAssetPath =
		TEXT("/Game/UEMMO/Rooms/DA_m5_068_missing.DA_m5_068_missing");

	// A deliberately missing enemy definition asset.
	const TCHAR* M5_068MissingEnemyAssetPath =
		TEXT("/Game/UEMMO/Enemy/Definitions/DA_m5_068_missing.DA_m5_068_missing");

	// One legal fallback wave (the recovery configuration of the zero-wave
	// case: a single enemy at a single location is a startable wave).
	static TArray<FRoomWaveDefinition> M5_068_MakeLegalWaves()
	{
		TArray<FRoomWaveDefinition> Waves;
		FRoomWaveDefinition Wave;
		Wave.EnemyId = FName(TEXT("melee_grunt"));
		Wave.Count = 1;
		Wave.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
		Waves.Add(Wave);
		return Waves;
	}

	// An illegal fallback wave: Count 2 with a single spawn location - the
	// spawner refuses this wave (RejectedInvalidRequest), which must fail the
	// freshly started run instead of leaving it waveless-Running.
	static TArray<FRoomWaveDefinition> M5_068_MakeIllegalWaves()
	{
		TArray<FRoomWaveDefinition> Waves;
		FRoomWaveDefinition Wave;
		Wave.EnemyId = FName(TEXT("melee_grunt"));
		Wave.Count = 2;
		Wave.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
		Waves.Add(Wave);
		return Waves;
	}

	// Recorded session events (the M2-009 precedent).
	struct FM5_068_RunEvents
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

	// One real, BeginPlay-initialized temp game world with the production
	// subsystem, a real ARoomTrigger at the fixed location and a real player
	// pawn that starts outside the activation volume (the M2-009 scene
	// precedent; overlaps dispatch through real world ticks).
	struct FM5_068_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		ARoomTrigger* Trigger = nullptr;
		APrototypeCharacter* Player = nullptr;

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
			if (!Test.TestNotNull(TEXT("the room session subsystem exists"), Session))
			{
				return false;
			}

			FActorSpawnParameters TriggerParams;
			Trigger = World->SpawnActor<ARoomTrigger>(
				ARoomTrigger::StaticClass(), FVector(M5_068TriggerLocationX, 0.0f, 88.0f),
				FRotator::ZeroRotator, TriggerParams);
			if (!Test.TestNotNull(TEXT("the room trigger spawns with production defaults"), Trigger))
			{
				return false;
			}

			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				FVector(M5_068OutsideLocationX, 0.0f, M5_068PlayerZ), FRotator::ZeroRotator, PlayerParams);
			if (!Test.TestNotNull(TEXT("the prototype character spawns"), Player))
			{
				return false;
			}
			if (!Player->HasActorBegunPlay())
			{
				Player->DispatchBeginPlay();
			}
			// Zero gravity + explicit overlap events: the pawn holds still in
			// the floor-less test world and the trigger sees its capsule.
			Player->GetCharacterMovement()->GravityScale = 0.0f;
			Player->GetCapsuleComponent()->SetGenerateOverlapEvents(true);
			TickTwice();
			return true;
		}

		void TickTwice()
		{
			Wrapper.TickTestWorld(0.016f);
			Wrapper.TickTestWorld(0.016f);
		}

		// Teleports the player and ticks so the overlap state settles (the
		// M2-009 move-and-tick precedent).
		void MoveAndTick(const FVector& Location)
		{
			Player->SetActorLocation(Location, /*bSweep*/ false);
			TickTwice();
			Player->GetCapsuleComponent()->UpdateOverlaps();
		}

		const FVector& InsideLocation() const
		{
			static const FVector Inside(M5_068TriggerLocationX, 0.0f, M5_068PlayerZ);
			return Inside;
		}

		const FVector& OutsideLocation() const
		{
			static const FVector Outside(M5_068OutsideLocationX, 0.0f, M5_068PlayerZ);
			return Outside;
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_068;

// 1. Acceptance: a missing enemy definition asset refuses the entry BEFORE
//    any run exists (session stays Idle, no start event, re-fireable); a
//    restored valid source lets the very next entry start the run normally,
//    and repeated overlaps then keep exactly one run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_068MissingEnemyAssetRefusesEntryUntilResourceRecovers,
	"UEMMO.Tasks.M5_068.MissingEnemyAssetRefusesEntryUntilResourceRecovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_068MissingEnemyAssetRefusesEntryUntilResourceRecovers::RunTest(const FString& Parameters)
{
	FM5_068_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM5_068_RunEvents Events;
	Events.Bind(*Scene.Session);

	// Break the enemy source; every entry attempt is refused pre-StartRoom.
	Scene.Trigger->SetEnemyDefinitionSource(
		TSoftObjectPtr<UEnemyDefinition>(FSoftObjectPath(M5_068MissingEnemyAssetPath)));
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("the entry with a missing enemy asset leaves the session Idle"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("no run was started"), Events.StartedCount, 0);
	TestEqual(TEXT("no run id exists"), Scene.Session->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("no wave was started"), Scene.Session->GetStartedWaveCount(), 0);

	// Repeated overlaps keep being refused-but-re-fireable: still Idle, still
	// no run, no state drift.
	for (int32 Cycle = 0; Cycle < 2; ++Cycle)
	{
		Scene.MoveAndTick(Scene.OutsideLocation());
		Scene.MoveAndTick(Scene.InsideLocation());
		TestTrue(TEXT("a re-entry while the source is broken stays Idle"),
			Scene.Session->GetState() == ERoomSessionState::Idle);
		TestEqual(TEXT("the broken source never started a run"), Events.StartedCount, 0);
	}

	// Restore the valid production source; the next entry starts normally.
	Scene.Trigger->SetEnemyDefinitionSource(
		TSoftObjectPtr<UEnemyDefinition>(FSoftObjectPath(M5_068ValidEnemyAssetPath)));
	Scene.MoveAndTick(Scene.OutsideLocation());
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("the entry after the resource recovered starts the run"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("exactly one start event fired"), Events.StartedCount, 1);
	TestTrue(TEXT("the run id was assigned"), Scene.Session->GetRunId() != 0);
	TestEqual(TEXT("the wave progression began (wave 0)"), Scene.Session->GetStartedWaveCount(), 1);

	// Repeated overlaps from here: the session's own rule keeps one run.
	const uint64 RunId = Scene.Session->GetRunId();
	Scene.MoveAndTick(Scene.OutsideLocation());
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("a re-entry keeps the recovered run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the re-entry started no second run"), Events.StartedCount, 1);
	TestEqual(TEXT("the re-entry kept the run id"), Scene.Session->GetRunId(), RunId);
	return true;
}

// 2. Acceptance: a room definition that does not resolve and a definition
//    without waves both refuse the entry the same way (Idle, re-fireable);
//    repairing the wave table lets the next entry start.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_068BrokenRoomDefinitionAndZeroWavesRefuseEntry,
	"UEMMO.Tasks.M5_068.BrokenRoomDefinitionAndZeroWavesRefuseEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_068BrokenRoomDefinitionAndZeroWavesRefuseEntry::RunTest(const FString& Parameters)
{
	FM5_068_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM5_068_RunEvents Events;
	Events.Bind(*Scene.Session);

	// A room definition asset that fails to load: refused before StartRoom.
	Scene.Trigger->SetRoomDefinitionSource(
		TSoftObjectPtr<URoomDefinition>(FSoftObjectPath(M5_068MissingRoomAssetPath)));
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("the entry with an unresolvable room definition stays Idle"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("no run was started by the broken room source"), Events.StartedCount, 0);

	// Back to the fallback path but with an empty wave table: still refused
	// (a run needs at least one wave).
	Scene.Trigger->SetRoomDefinitionSource(TSoftObjectPtr<URoomDefinition>());
	Scene.Trigger->SetFallbackWaves(TArray<FRoomWaveDefinition>());
	Scene.MoveAndTick(Scene.OutsideLocation());
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("the entry with a zero-wave definition stays Idle"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the zero-wave definition started no run"), Events.StartedCount, 0);
	TestEqual(TEXT("no run id was assigned"), Scene.Session->GetRunId(), static_cast<uint64>(0));

	// Repairing the wave table: the next entry starts normally.
	Scene.Trigger->SetFallbackWaves(M5_068_MakeLegalWaves());
	Scene.MoveAndTick(Scene.OutsideLocation());
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("the entry after repairing the wave table starts the run"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("exactly one start event fired"), Events.StartedCount, 1);
	TestEqual(TEXT("the wave progression began (wave 0)"), Scene.Session->GetStartedWaveCount(), 1);
	return true;
}

// 3. Acceptance: an accepted run whose BeginWaves is refused (an illegal
//    first wave) ends Failed - never a waveless Running session: no kills,
//    no births, no pending spawns, no reward eligibility, exactly one end
//    event, and re-entries stay inert on the Failed outcome.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_068RefusedBeginWavesEndsFailedNotWavelessRunning,
	"UEMMO.Tasks.M5_068.RefusedBeginWavesEndsFailedNotWavelessRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_068RefusedBeginWavesEndsFailedNotWavelessRunning::RunTest(const FString& Parameters)
{
	FM5_068_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM5_068_RunEvents Events;
	Events.Bind(*Scene.Session);

	// A wave table whose first wave is illegal (count 2, one location): the
	// entry validation passes (the definition resolves and carries waves),
	// StartRoom is accepted, and the spawner then refuses the wave.
	Scene.Trigger->SetFallbackWaves(M5_068_MakeIllegalWaves());
	Scene.MoveAndTick(Scene.InsideLocation());

	const uint64 RunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the run id was assigned before the refusal"), RunId != 0);
	TestTrue(TEXT("the refused BeginWaves ended the run Failed (not waveless Running)"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("exactly one start event fired"), Events.StartedCount, 1);
	TestEqual(TEXT("exactly one end event fired"), Events.EndedCount, 1);
	if (TestTrue(TEXT("the end event carried one result"), Events.EndedResults.Num() == 1))
	{
		TestFalse(TEXT("the result is not Cleared"), Events.EndedResults[0].bCleared);
		TestEqual(TEXT("the result carries the failed run id"), Events.EndedResults[0].RunId, RunId);
		TestEqual(TEXT("the result counts zero kills"), Events.EndedResults[0].KilledCount, 0);
	}
	TestEqual(TEXT("no enemy was ever born"), Scene.Session->GetSpawnedEnemyCount(), 0);
	TestEqual(TEXT("no unborn spawn remains queued"), Scene.Session->GetRunPendingSpawnCount(), 0);
	TestEqual(TEXT("no wave was ever recorded"), Scene.Session->GetStartedWaveCount(), 0);
	TestFalse(TEXT("the failed run is not reward eligible"), Scene.Session->IsRewardEligible(RunId));

	// Re-entries stay inert on the Failed outcome: no second run, no second
	// end event (the session's StartRoom rule rejects from Failed).
	Scene.MoveAndTick(Scene.OutsideLocation());
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("a re-entry keeps the session Failed"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the re-entry started no second run"), Events.StartedCount, 1);
	TestEqual(TEXT("the re-entry fired no second end event"), Events.EndedCount, 1);
	return true;
}

#endif
