// M5-073: the production rule wiring. A trigger whose room definition asset
// carries rules (the real DA_room_combat_01) drives the run through the
// M5-072 evaluator: the entry publishes the activation event, the session's
// confirmed WaveCleared / PlayerDied facts become rule events, StartWave(0)
// starts the SAME BeginWaves engine, and the trusted retry service's fresh
// run re-publishes the activation (retry-marked, no second overlap) - the
// observable gameplay stays exactly the 2+3 waves with the 1 s inter-wave
// gap, the Y-depth activation volume, the death failure and the retry.
// Rule-less triggers keep the unchanged direct path; the training map keeps
// no trigger at all.
//
// The scenes follow the M2-016 precedent: a real, BeginPlay-initialized temp
// game world whose ONLY time source is the production character-tick clock
// injection (no SetSessionClockSeconds call anywhere), real enemies killed
// through their health components, and a real player killed through its
// health pool.

#include "Misc/AutomationTest.h"

#include <limits>

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomRetryService.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/RoomTrigger.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_073
{
	constexpr float M5_073_TriggerLocationX = -100.0f;
	constexpr float M5_073_OutsideLocationX = -800.0f;
	constexpr float M5_073_PlayerZ = 100.0f;
	const TCHAR* M5_073_RoomAssetPath = TEXT("/Game/UEMMO/Rooms/DA_room_combat_01.DA_room_combat_01");
	const TCHAR* M5_073_EnemyAssetPath = TEXT("/Game/UEMMO/Enemy/Definitions/DA_melee_grunt.DA_melee_grunt");
	const TCHAR* M5_073_TrainingMapPath = TEXT("/Game/UEMMO/Maps/L_TrainingArena.L_TrainingArena");

	/** One temp game world with the production subsystem, a real player pawn and a real ARoomTrigger (the M2-016 scene precedent). */
	struct FM5_073_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		ARoomTrigger* Trigger = nullptr;

		bool Build(FAutomationTestBase& Test, bool bRuleAsset)
		{
			if (!Test.TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World)
				|| !Test.TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists"), Session))
			{
				return false;
			}

			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				FVector(M5_073_OutsideLocationX, 0.0f, M5_073_PlayerZ), FRotator::ZeroRotator, PlayerParams);
			if (!Test.TestNotNull(TEXT("the prototype character spawns"), Player))
			{
				return false;
			}
			if (!Player->HasActorBegunPlay())
			{
				Player->DispatchBeginPlay();
			}
			Player->GetCharacterMovement()->GravityScale = 0.0f;
			Player->GetCapsuleComponent()->SetGenerateOverlapEvents(true);

			FActorSpawnParameters TriggerParams;
			Trigger = World->SpawnActor<ARoomTrigger>(
				ARoomTrigger::StaticClass(), FVector(M5_073_TriggerLocationX, 0.0f, 88.0f),
				FRotator::ZeroRotator, TriggerParams);
			if (!Test.TestNotNull(TEXT("the room trigger spawns"), Trigger))
			{
				return false;
			}
			if (bRuleAsset)
			{
				Trigger->SetRoomDefinitionSource(
					TSoftObjectPtr<URoomDefinition>(FSoftObjectPath(M5_073_RoomAssetPath)));
			}
			TickTwice();
			return true;
		}

		void TickTwice()
		{
			Wrapper.TickTestWorld(0.016f);
			Wrapper.TickTestWorld(0.016f);
		}

		bool AdvanceSeconds(FAutomationTestBase& Test, double Seconds)
		{
			// Real 60 fps steps: the session clock moves only through the
			// production character-tick injection.
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double Step = FMath::Min(1.0 / 60.0, Remaining);
				if (!Test.TestTrue(TEXT("the world ticks"), Wrapper.TickTestWorld(static_cast<float>(Step))))
				{
					return false;
				}
				Remaining -= Step;
			}
			return true;
		}

		void MoveAndTick(const FVector& Location)
		{
			Player->SetActorLocation(Location, /*bSweep*/ false);
			TickTwice();
			Player->GetCapsuleComponent()->UpdateOverlaps();
		}

		const FVector InsideLocation() const
		{
			return FVector(M5_073_TriggerLocationX, 0.0f, M5_073_PlayerZ);
		}

		const FVector OutsideLocation() const
		{
			return FVector(M5_073_OutsideLocationX, 0.0f, M5_073_PlayerZ);
		}

		int32 AliveEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				UHealthComponent* Health = It->GetHealthComponent();
				if (Health != nullptr && Health->IsAlive())
				{
					++Count;
				}
			}
			return Count;
		}

		AMeleeEnemy* FindAliveEnemyAt(const FVector& Location) const
		{
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				UHealthComponent* Health = It->GetHealthComponent();
				if (Health == nullptr || !Health->IsAlive())
				{
					continue;
				}
				if (FVector::DistSquared(It->GetActorLocation(), Location) < 4.0)
				{
					return *It;
				}
			}
			return nullptr;
		}

		/**
		 * Kills every ALIVE enemy through the legal lethal path. The enemies
		 * of a world with a player pawn chase after their spawn grace, so the
		 * kills address the alive set (the wave membership is pinned by the
		 * count), never their spawn coordinates.
		 */
		bool KillAllAliveEnemies(FAutomationTestBase& Test, int32 ExpectedCount)
		{
			if (!Test.TestEqual(TEXT("the alive enemy count matches the expectation before the kill"),
				AliveEnemies(), ExpectedCount))
			{
				return false;
			}
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				UHealthComponent* Health = It->GetHealthComponent();
				if (Health != nullptr && Health->IsAlive())
				{
					Health->ApplyDamage(999.0f);
				}
			}
			return Test.TestEqual(TEXT("no alive enemy remains after the kill"), AliveEnemies(), 0);
		}

		bool KillPlayer(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the player health pool accepts the lethal damage"),
				Player->GetHealth()->ApplyDamage(9999.0f) > 0.0f))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the player health pool is dead"),
				!Player->GetHealth()->IsAlive());
		}
	};

	/** Recorded session events (the assertion surface). */
	struct FM5_073_RunEvents
	{
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;

		void Bind(URoomSessionSubsystem& Session)
		{
			Session.OnRunStarted().AddLambda([this]() { ++StartedCount; });
			Session.OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_073;

// 1. Acceptance: the rule-driven production run plays exactly the existing
//    room game - entry (Y-depth gated) starts ONLY wave 0 with its two
//    enemies, their confirmed clear arms the 1 s gap before wave 1's three,
//    the full clear settles Cleared once; the player death fails the run and
//    the trusted retry starts a fresh run (new RunId/SettlementId, retry
//    activation without a second overlap) that clears 2+3 again; repeated
//    overlaps stay inert throughout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_073RulePathEntryWavesDeathAndRetry,
	"UEMMO.Tasks.M5_073.RulePathEntryWavesDeathAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_073RulePathEntryWavesDeathAndRetry::RunTest(const FString& Parameters)
{
	URoomDefinition* Room = LoadObject<URoomDefinition>(nullptr, M5_073_RoomAssetPath);
	if (!TestNotNull(TEXT("DA_room_combat_01 loads (run Scripts/PrepareRooms.ps1)"), Room))
	{
		return true;
	}
	UEnemyDefinition* EnemyDef = LoadObject<UEnemyDefinition>(nullptr, M5_073_EnemyAssetPath);
	if (!TestNotNull(TEXT("DA_melee_grunt loads"), EnemyDef))
	{
		return true;
	}

	FM5_073_Scene Scene;
	if (!Scene.Build(*this, /*bRuleAsset*/ true))
	{
		return true;
	}
	FM5_073_RunEvents Events;
	Events.Bind(*Scene.Session);

	// Y-depth negative case: the same X as the trigger volume but far outside
	// its Y extent - no run, no rule path.
	Scene.MoveAndTick(FVector(M5_073_TriggerLocationX, 400.0f, M5_073_PlayerZ));
	TestTrue(TEXT("a player outside the Y depth of the volume starts nothing"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("no run started from outside"), Events.StartedCount, 0);
	TestFalse(TEXT("the rule path stays inactive outside"), Scene.Trigger->IsRulePathActive());

	// Y-depth positive case: entering the volume starts the rule-driven run.
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("entering the volume starts the run"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestTrue(TEXT("the rule path is active"), Scene.Trigger->IsRulePathActive());
	TestEqual(TEXT("exactly one start event"), Events.StartedCount, 1);

	// Only wave 0 with its two enemies is live at the start.
	if (!Scene.AdvanceSeconds(*this, 0.6))
	{
		return true;
	}
	TestEqual(TEXT("the rule run birthed exactly the two wave-0 enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	TestEqual(TEXT("only wave 0 is current"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("two enemies are alive"), Scene.AliveEnemies(), 2);

	// Repeated overlaps (out and back in) change nothing.
	const uint64 FirstRunId = Scene.Session->GetRunId();
	Scene.MoveAndTick(Scene.OutsideLocation());
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("a repeated overlap keeps the run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("no second run started"), Events.StartedCount, 1);
	TestEqual(TEXT("the run id survived the overlap"), Scene.Session->GetRunId(), FirstRunId);

	// Both wave-0 deaths: before the 1 s gap elapses wave 1 must NOT start.
	if (!Scene.KillAllAliveEnemies(*this, /*ExpectedCount*/ 2))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 kills counted"), Scene.Session->GetKilledCount(), 2);
	if (!Scene.AdvanceSeconds(*this, 0.5))
	{
		return true;
	}
	TestEqual(TEXT("wave 1 did not start before the 1 s gap"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("no wave-1 enemy was born early"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// After the full gap wave 1 starts and births its three enemies.
	if (!Scene.AdvanceSeconds(*this, 1.1))
	{
		return true;
	}
	TestEqual(TEXT("wave 1 started after the 1 s gap"), Scene.Session->GetCurrentWaveIndex(), 1);
	if (!Scene.AdvanceSeconds(*this, 1.0))
	{
		return true;
	}
	TestEqual(TEXT("all five enemies were born"), Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("three wave-1 enemies are alive"), Scene.AliveEnemies(), 3);

	// The full clear settles exactly once. (The completed-count is captured
	// BEFORE the final kill: the terminal settlement drops the rule scope, so
	// the post-clear count accessor intentionally reports no active scope.)
	const int32 CompletedRulesBeforeFinalKill = Scene.Trigger->GetRuleCompletedCount();
	if (!Scene.KillAllAliveEnemies(*this, /*ExpectedCount*/ 3))
	{
		return true;
	}
	TestTrue(TEXT("the rule run cleared"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("exactly one end event"), Events.EndedCount, 1);
	if (TestTrue(TEXT("the result snapshot exists"), Events.EndedResults.Num() == 1))
	{
		TestTrue(TEXT("the result is Cleared"), Events.EndedResults[0].bCleared);
		TestEqual(TEXT("all five deaths counted"), Events.EndedResults[0].KilledCount, 5);
	}
	TestTrue(TEXT("rule once-effects completed through the run"),
		CompletedRulesBeforeFinalKill >= 2);
	TestTrue(TEXT("events flowed through the evaluator"),
		Scene.Trigger->GetRuleEventSequence() >= 3);

	// -- The retry sub-scenario (a fresh scene: a Cleared run cannot retry) --
	FM5_073_Scene RetryScene;
	if (!RetryScene.Build(*this, /*bRuleAsset*/ true))
	{
		return true;
	}
	FM5_073_RunEvents RetryEvents;
	RetryEvents.Bind(*RetryScene.Session);
	RetryScene.MoveAndTick(RetryScene.InsideLocation());
	if (!RetryScene.AdvanceSeconds(*this, 0.6))
	{
		return true;
	}
	TestEqual(TEXT("the retry scene birthed wave 0"), RetryScene.Session->GetSpawnedEnemyCount(), 2);
	const uint64 FailedRunId = RetryScene.Session->GetRunId();
	const uint64 FailedSettlementId = RetryScene.Session->GetSettlementId();

	// The player death fails the run.
	if (!RetryScene.KillPlayer(*this))
	{
		return true;
	}
	TestTrue(TEXT("the player death failed the run"),
		RetryScene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the failed run fired one end event"), RetryEvents.EndedCount, 1);
	TestFalse(TEXT("the failed result is not Cleared"), RetryEvents.EndedResults[0].bCleared);

	// The trusted retry: a fresh run WITHOUT a second physical overlap.
	if (!TestTrue(TEXT("the retry service starts the fresh run"),
		URoomRetryService::RetryRoom(RetryScene.World, Room, EnemyDef, RetryScene.Player)))
	{
		return true;
	}
	const uint64 RetryRunId = RetryScene.Session->GetRunId();
	TestTrue(TEXT("the retry run got a new run id"), RetryRunId != FailedRunId);
	TestTrue(TEXT("the retry run got a new settlement id"),
		RetryScene.Session->GetSettlementId() != FailedSettlementId);
	TestEqual(TEXT("the retry fired a second start event"), RetryEvents.StartedCount, 2);
	TestTrue(TEXT("the retry run is Running"),
		RetryScene.Session->GetState() == ERoomSessionState::Running);
	TestTrue(TEXT("the rule path re-armed for the retry run"), RetryScene.Trigger->IsRulePathActive());
	const uint64 SequenceBeforeRetryPublish = RetryScene.Trigger->GetRuleEventSequence();
	if (!RetryScene.AdvanceSeconds(*this, 0.1))
	{
		return true;
	}
	TestTrue(TEXT("the retry activation event was published (no second overlap)"),
		RetryScene.Trigger->GetRuleEventSequence() > SequenceBeforeRetryPublish);
	if (!RetryScene.AdvanceSeconds(*this, 0.6))
	{
		return true;
	}
	TestEqual(TEXT("the retry run birthed its own wave 0"), RetryScene.Session->GetSpawnedEnemyCount(), 2);

	// The retry run clears the full 2+3 as well.
	if (!RetryScene.KillAllAliveEnemies(*this, /*ExpectedCount*/ 2))
	{
		return true;
	}
	if (!RetryScene.AdvanceSeconds(*this, 2.2))
	{
		return true;
	}
	if (!RetryScene.KillAllAliveEnemies(*this, /*ExpectedCount*/ 3))
	{
		return true;
	}
	TestTrue(TEXT("the retry run cleared"), RetryScene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the retry settled exactly once more"), RetryEvents.EndedCount, 2);
	if (RetryEvents.EndedResults.Num() == 2)
	{
		TestEqual(TEXT("the retry result carries the new run id"), RetryEvents.EndedResults[1].RunId, RetryRunId);
	}
	return true;
}

// 2. Acceptance: rule-less triggers keep the unchanged direct BeginWaves
//    path (the test fixtures' contract), and the training map still carries
//    no trigger (no auto-spawning arena).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_073NonRuleDirectPathAndTrainingStaysClean,
	"UEMMO.Tasks.M5_073.NonRuleDirectPathAndTrainingStaysClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_073NonRuleDirectPathAndTrainingStaysClean::RunTest(const FString& Parameters)
{
	FM5_073_Scene Scene;
	if (!Scene.Build(*this, /*bRuleAsset*/ false))
	{
		return true;
	}
	FM5_073_RunEvents Events;
	Events.Bind(*Scene.Session);

	// The class-default trigger (fallback waves, no rules): the direct path.
	Scene.MoveAndTick(Scene.InsideLocation());
	TestTrue(TEXT("the rule-less entry starts the run"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the direct path started wave 0"), Scene.Session->GetStartedWaveCount(), 1);
	TestFalse(TEXT("the rule path stays inactive for rule-less definitions"),
		Scene.Trigger->IsRulePathActive());
	if (!Scene.AdvanceSeconds(*this, 0.6))
	{
		return true;
	}
	TestEqual(TEXT("the direct path birthed its two wave-0 enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);

	// The training map keeps no trigger at all (its render-only semantics).
	UWorld* TrainingMap = LoadObject<UWorld>(nullptr, M5_073_TrainingMapPath);
	if (!TestNotNull(TEXT("the training map asset loads"), TrainingMap))
	{
		return true;
	}
	int32 TriggerCount = 0;
	if (TrainingMap->PersistentLevel != nullptr)
	{
		for (AActor* Actor : TrainingMap->PersistentLevel->Actors)
		{
			if (Actor != nullptr && Actor->IsA(ARoomTrigger::StaticClass()))
			{
				++TriggerCount;
			}
		}
	}
	TestEqual(TEXT("the training map carries no room trigger"), TriggerCount, 0);
	return true;
}

#endif
