// M2-013: one-time settlement identity and immutable run results. Every
// finished run's result is snapshotted once at its terminal transition and
// afterwards handed out ONLY as value copies: a repeated GetRunResult of the
// same run returns the identical RunId / SettlementId / Seed (nothing is
// re-generated or re-randomized), a Failed result never enters the reward
// gate (IsRewardEligible is Cleared-only), the next run settles under a
// different SettlementId, non-terminal and unknown runs are refused, and
// mutating a returned copy can never touch the session's internal state.
// The suite also re-asserts the core M2-006 result semantics (OnRunEnded
// exactly once with the full result, first terminal wins) to prove the
// FRoomResult migration into RoomResult.h changed nothing.
//
// The scaffolding reuses the RoomSessionTests pattern: a fresh temp game
// world per test, the production URoomSessionSubsystem fetched through the
// world's subsystem collection, and the injected session clock (no wall
// clock, no ticking).

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"

#include "../Room/RoomDefinition.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_013
{
	// One fresh game world per need; names are uniquified because several
	// worlds of this suite can be alive inside the same process at once.
	static UWorld* M2_013_AcquireTestWorld()
	{
		static int32 M2_013_WorldCounter = 0;
		++M2_013_WorldCounter;
		const FName WorldName = FName(*FString::Printf(TEXT("M2_013_TestWorld_%d"), M2_013_WorldCounter));
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
		if (World != nullptr)
		{
			return World;
		}
		UE_LOG(LogTemp, Log, TEXT("M2_013 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Subsystem fetch through the world's collection (the production access
	// path); reports and returns null when the world has none.
	static URoomSessionSubsystem* M2_013_GetSession(FAutomationTestBase& Test, UWorld* World)
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

	// Minimal room definition: StartRoom only reads the room id for the
	// result; wave/map data is not needed by the settlement queries.
	static URoomDefinition* M2_013_MakeRoom(FName RoomId)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = RoomId;
		return Room;
	}

	// Recorded session events (the assertions read this struct only).
	struct FM2_013_RunEvents
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
}

using namespace UE::UEMMO::Tasks::M2_013;

// 1. The same finished run queried twice returns the identical result:
//    RunId, SettlementId and Seed are stable - nothing is re-generated or
//    re-randomized by the queries.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013SameRunResultQueriedTwiceKeepsIdAndSeed,
	"UEMMO.Tasks.M2_013.SameRunResultQueriedTwiceKeepsIdAndSeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013SameRunResultQueriedTwiceKeepsIdAndSeed::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	Session->SetSessionClockSeconds(5.0);
	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();
	TestTrue(TEXT("a mid-run kill is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	Session->SetSessionClockSeconds(17.5);
	if (!TestTrue(TEXT("the run clears"), Session->MarkCleared()))
	{
		return true;
	}

	// First query: the full result snapshot, every field populated.
	FRoomResult First;
	if (!TestTrue(TEXT("the finished run's result is available"), Session->GetRunResult(RunId, First)))
	{
		return true;
	}
	TestEqual(TEXT("the result carries the run id"), First.RunId, RunId);
	TestTrue(TEXT("the result carries a nonzero settlement id"), First.SettlementId > 0);
	TestEqual(TEXT("the result carries the room id"), First.RoomId, FName(TEXT("room_test_01")));
	TestTrue(TEXT("the result carries a nonzero seed"), First.Seed != 0);
	TestTrue(TEXT("the result records Cleared"), First.bCleared);
	TestEqual(TEXT("the result counts the kill"), First.KilledCount, 1);
	TestEqual(TEXT("the result measures elapsed seconds with the injected clock"), First.ElapsedSeconds, 12.5);

	// Second query of the SAME run: identical identity, no re-generation.
	FRoomResult Second;
	TestTrue(TEXT("a second request for the same run succeeds too"), Session->GetRunResult(RunId, Second));
	TestEqual(TEXT("the second copy carries the same run id"), Second.RunId, First.RunId);
	TestEqual(TEXT("the second copy carries the same settlement id (nothing re-generated)"),
		Second.SettlementId, First.SettlementId);
	TestEqual(TEXT("the second copy carries the same seed (nothing re-randomized)"), Second.Seed, First.Seed);
	TestEqual(TEXT("the second copy carries the same room id"), Second.RoomId, First.RoomId);
	TestTrue(TEXT("the second copy records the same outcome"), Second.bCleared == First.bCleared);
	TestEqual(TEXT("the second copy counts the same kills"), Second.KilledCount, First.KilledCount);
	TestEqual(TEXT("the second copy measures the same elapsed time"), Second.ElapsedSeconds, First.ElapsedSeconds);

	// The settlement query is equally one-time stable.
	TestEqual(TEXT("the settlement query returns the result's settlement id"),
		Session->GetSettlementId(RunId), First.SettlementId);
	TestEqual(TEXT("a repeated settlement query returns the same id"),
		Session->GetSettlementId(RunId), Session->GetSettlementId(RunId));
	return true;
}

// 2. A Failed result is never reward eligible: Cleared=false and the gate
//    refuses the run on every request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013FailedResultIsNotRewardEligible,
	"UEMMO.Tasks.M2_013.FailedResultIsNotRewardEligible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013FailedResultIsNotRewardEligible::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();
	if (!TestTrue(TEXT("the run fails"), Session->FailRun()))
	{
		return true;
	}

	FRoomResult Result;
	TestTrue(TEXT("the failed run's result is available"), Session->GetRunResult(RunId, Result));
	TestFalse(TEXT("the failed result records Cleared=false"), Result.bCleared);
	TestFalse(TEXT("the failed result never enters the reward gate"),
		Session->IsRewardEligible(RunId));
	TestFalse(TEXT("the reward gate refuses the failed run again on a repeated request"),
		Session->IsRewardEligible(RunId));
	// A failed run still settles: its one-time settlement id stays queryable.
	TestTrue(TEXT("the failed run carries a nonzero settlement id"), Result.SettlementId > 0);
	TestEqual(TEXT("the settlement query answers the failed run's settled id"),
		Session->GetSettlementId(RunId), Result.SettlementId);
	return true;
}

// 3. A Cleared result is the only reward-eligible outcome.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013ClearedResultIsRewardEligible,
	"UEMMO.Tasks.M2_013.ClearedResultIsRewardEligible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013ClearedResultIsRewardEligible::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();
	if (!TestTrue(TEXT("the run clears"), Session->MarkCleared()))
	{
		return true;
	}

	FRoomResult Result;
	TestTrue(TEXT("the cleared run's result is available"), Session->GetRunResult(RunId, Result));
	TestTrue(TEXT("the cleared result records Cleared=true"), Result.bCleared);
	TestTrue(TEXT("the cleared result is reward eligible"), Session->IsRewardEligible(RunId));
	TestTrue(TEXT("the reward gate stays open on a repeated request"),
		Session->IsRewardEligible(RunId));
	return true;
}

// 4. The next run settles under a different SettlementId (a settlement id is
//    minted exactly once and never reused), and the previous run's archived
//    result stays queryable with its original id after the reset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013NextRunGetsDifferentSettlementId,
	"UEMMO.Tasks.M2_013.NextRunGetsDifferentSettlementId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013NextRunGetsDifferentSettlementId::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the first run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 FirstRunId = Session->GetRunId();
	if (!TestTrue(TEXT("the first run fails"), Session->FailRun()))
	{
		return true;
	}
	FRoomResult First;
	if (!TestTrue(TEXT("the first run's result is available"), Session->GetRunResult(FirstRunId, First)))
	{
		return true;
	}
	TestTrue(TEXT("the first run carries a nonzero settlement id"), First.SettlementId > 0);

	if (!TestTrue(TEXT("the session resets to Idle"), Session->ResetToIdle()))
	{
		return true;
	}
	if (!TestTrue(TEXT("the second run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 SecondRunId = Session->GetRunId();
	if (!TestTrue(TEXT("the second run clears"), Session->MarkCleared()))
	{
		return true;
	}

	FRoomResult Second;
	TestTrue(TEXT("the second run's result is available"), Session->GetRunResult(SecondRunId, Second));
	TestTrue(TEXT("the next run carries a different run id"), SecondRunId != FirstRunId);
	TestTrue(TEXT("the next run settles under a different settlement id"),
		Second.SettlementId != First.SettlementId);
	TestEqual(TEXT("the second run's settlement query agrees with its result"),
		Session->GetSettlementId(SecondRunId), Second.SettlementId);

	// The archive survives the reset by design: the first run's settled
	// identity is still answerable, unchanged, after the session moved on.
	FRoomResult FirstAgain;
	TestTrue(TEXT("the previous run's result stays queryable after the reset"),
		Session->GetRunResult(FirstRunId, FirstAgain));
	TestEqual(TEXT("the archived first result keeps its settlement id"),
		FirstAgain.SettlementId, First.SettlementId);
	TestEqual(TEXT("the archived first result keeps its seed"), FirstAgain.Seed, First.Seed);
	TestEqual(TEXT("the first run's settlement query still answers the same id"),
		Session->GetSettlementId(FirstRunId), First.SettlementId);
	return true;
}

// 5. GetRunResult returns a VALUE copy: mutating the caller-owned copy can
//    never touch the session's internal state (immutable hand-out).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013RunResultCopyIsValueSemantics,
	"UEMMO.Tasks.M2_013.RunResultCopyIsValueSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013RunResultCopyIsValueSemantics::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();
	TestTrue(TEXT("a mid-run kill is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	if (!TestTrue(TEXT("the run clears"), Session->MarkCleared()))
	{
		return true;
	}

	FRoomResult Copy;
	if (!TestTrue(TEXT("the run result is available"), Session->GetRunResult(RunId, Copy)))
	{
		return true;
	}

	// The caller owns the copy: scribble over every field of it.
	Copy.RunId = 9999;
	Copy.SettlementId = 8888;
	Copy.RoomId = FName(TEXT("mutated"));
	Copy.Seed = -123;
	Copy.bCleared = false;
	Copy.KilledCount = 77;
	Copy.ElapsedSeconds = -1.0;

	// The internal state is untouched: a fresh query and the last-result
	// introspection both still carry the original snapshot.
	FRoomResult Fresh;
	TestTrue(TEXT("a fresh query after the mutation still succeeds"),
		Session->GetRunResult(RunId, Fresh));
	TestEqual(TEXT("the internal run id survived the mutated copy"), Fresh.RunId, RunId);
	TestTrue(TEXT("the internal settlement id survived the mutated copy"),
		Fresh.SettlementId > 0 && Fresh.SettlementId != 8888);
	TestEqual(TEXT("the internal room id survived the mutated copy"),
		Fresh.RoomId, FName(TEXT("room_test_01")));
	TestTrue(TEXT("the internal seed survived the mutated copy"), Fresh.Seed != -123 && Fresh.Seed != 0);
	TestTrue(TEXT("the internal outcome survived the mutated copy"), Fresh.bCleared);
	TestEqual(TEXT("the internal kill count survived the mutated copy"), Fresh.KilledCount, 1);
	TestTrue(TEXT("the internal elapsed time survived the mutated copy"), Fresh.ElapsedSeconds >= 0.0);
	TestTrue(TEXT("GetLastResult is untouched by the mutated copy"),
		Session->GetLastResult().RunId == RunId && Session->GetLastResult().bCleared
		&& Session->GetLastResult().KilledCount == 1);
	return true;
}

// 6. Non-terminal and unknown runs are refused: a still-Running run has no
//    result and no settlement id yet, and neither does an unknown (or 0) id
//    after a terminal state - while the real settled run keeps resolving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013NonTerminalOrUnknownRunIsRefused,
	"UEMMO.Tasks.M2_013.NonTerminalOrUnknownRunIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013NonTerminalOrUnknownRunIsRefused::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();

	// While Running the run has NOT settled: refused everywhere, and the out
	// value stays default-constructed (the sentinel proves it was touched).
	FRoomResult NotYet;
	NotYet.RunId = 4242;
	TestFalse(TEXT("a result query for the still-running run is rejected"),
		Session->GetRunResult(RunId, NotYet));
	TestEqual(TEXT("the rejected query left the out value empty (run id 0)"),
		NotYet.RunId, static_cast<uint64>(0));
	TestEqual(TEXT("the settlement query of the running run returns 0"),
		Session->GetSettlementId(RunId), static_cast<uint64>(0));
	TestFalse(TEXT("the running run is not reward eligible yet"),
		Session->IsRewardEligible(RunId));

	if (!TestTrue(TEXT("the run clears"), Session->MarkCleared()))
	{
		return true;
	}

	// After the terminal state, unknown ids (and the 0 "no run" id) are still
	// refused everywhere.
	FRoomResult Unknown;
	Unknown.RunId = 4242;
	TestFalse(TEXT("a result query for an unknown run id is rejected"),
		Session->GetRunResult(9999, Unknown));
	TestEqual(TEXT("the unknown-run query left the out value empty"),
		Unknown.RunId, static_cast<uint64>(0));
	TestEqual(TEXT("the settlement query of an unknown run returns 0"),
		Session->GetSettlementId(9999), static_cast<uint64>(0));
	TestFalse(TEXT("an unknown run is never reward eligible"), Session->IsRewardEligible(9999));
	TestFalse(TEXT("run id 0 (no run) is never a result"), Session->GetRunResult(0, Unknown));
	TestEqual(TEXT("the settlement query of run id 0 returns 0"),
		Session->GetSettlementId(0), static_cast<uint64>(0));
	TestFalse(TEXT("run id 0 is never reward eligible"), Session->IsRewardEligible(0));

	// The really settled run keeps resolving next to all the refusals.
	FRoomResult Settled;
	TestTrue(TEXT("the actually settled run still resolves"),
		Session->GetRunResult(RunId, Settled));
	TestEqual(TEXT("the settled result carries the run id"), Settled.RunId, RunId);
	TestTrue(TEXT("the settled run carries a nonzero settlement id"), Settled.SettlementId > 0);
	return true;
}

// 7. M2-006 regression after the FRoomResult migration: OnRunEnded fires
//    exactly once per run with the full result, first terminal wins (late
//    duplicates never overwrite or re-fire), GetLastResult matches, and the
//    M2-013 archive agrees with the broadcast result across two runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_013M2_006ResultSemanticsSurviveResultHeaderMigration,
	"UEMMO.Tasks.M2_013.M2_006ResultSemanticsSurviveResultHeaderMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_013M2_006ResultSemanticsSurviveResultHeaderMigration::RunTest(const FString& Parameters)
{
	UWorld* World = M2_013_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_013_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_013_RunEvents Events;
	Events.Bind(*Session);

	// Run 1: cleared, with kills and a measured elapsed time.
	Session->SetSessionClockSeconds(10.0);
	const URoomDefinition* Room = M2_013_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the first run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 FirstRunId = Session->GetRunId();
	const uint64 FirstSettlementId = Session->GetSettlementId();
	const int32 FirstSeed = Session->GetSeed();
	TestTrue(TEXT("a mid-run kill is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestTrue(TEXT("a second mid-run kill is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	Session->SetSessionClockSeconds(32.5);
	if (!TestTrue(TEXT("the first run clears"), Session->MarkCleared()))
	{
		return true;
	}

	// The M2-006 event semantics: exactly one end event with the full result.
	TestTrue(TEXT("the session is Cleared after MarkCleared"),
		Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("OnRunEnded fired exactly once"), Events.EndedCount, 1);
	if (TestTrue(TEXT("the end event carried one result"), Events.EndedResults.Num() == 1))
	{
		const FRoomResult& Result = Events.EndedResults[0];
		TestTrue(TEXT("the broadcast result records Cleared"), Result.bCleared);
		TestEqual(TEXT("the broadcast result carries the run id"), Result.RunId, FirstRunId);
		TestEqual(TEXT("the broadcast result carries the settlement id"), Result.SettlementId, FirstSettlementId);
		TestEqual(TEXT("the broadcast result carries the seed"), Result.Seed, FirstSeed);
		TestEqual(TEXT("the broadcast result carries the room id"), Result.RoomId, FName(TEXT("room_test_01")));
		TestEqual(TEXT("the broadcast result counts both kills"), Result.KilledCount, 2);
		TestEqual(TEXT("the broadcast result measures elapsed seconds"), Result.ElapsedSeconds, 22.5);
	}
	TestTrue(TEXT("GetLastResult still matches the broadcast result"),
		Session->GetLastResult().RunId == FirstRunId && Session->GetLastResult().bCleared
		&& Session->GetLastResult().KilledCount == 2);

	// First terminal wins: duplicates are refused and re-fire nothing.
	TestFalse(TEXT("FailRun after Cleared is rejected"), Session->FailRun());
	TestFalse(TEXT("a second MarkCleared is rejected too"), Session->MarkCleared());
	TestEqual(TEXT("OnRunEnded still fired exactly once"), Events.EndedCount, 1);
	TestFalse(TEXT("a stale kill after Cleared is ignored"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));

	// The M2-013 archive agrees with the broadcast result of the same run.
	FRoomResult Archived;
	TestTrue(TEXT("the archive resolves the same run"), Session->GetRunResult(FirstRunId, Archived));
	TestEqual(TEXT("the archived result carries the broadcast settlement id"),
		Archived.SettlementId, FirstSettlementId);
	TestEqual(TEXT("the archived result carries the broadcast seed"), Archived.Seed, FirstSeed);
	TestTrue(TEXT("the archived result records the same outcome"), Archived.bCleared);
	TestEqual(TEXT("the archived result counts the same kills"), Archived.KilledCount, 2);

	// Run 2: failed - the end event fires again (once) for the new run only,
	// and run 1's recorded outcome is never overwritten.
	if (!TestTrue(TEXT("ResetToIdle from the terminal state is accepted"), Session->ResetToIdle()))
	{
		return true;
	}
	if (!TestTrue(TEXT("the second run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 SecondRunId = Session->GetRunId();
	if (!TestTrue(TEXT("the second run fails"), Session->FailRun()))
	{
		return true;
	}
	TestEqual(TEXT("the second run added exactly one end event"), Events.EndedCount, 2);
	if (TestTrue(TEXT("the second end event carried one new result"), Events.EndedResults.Num() == 2))
	{
		const FRoomResult& Second = Events.EndedResults[1];
		TestFalse(TEXT("the second run's result records Failed"), Second.bCleared);
		TestEqual(TEXT("the second run's result carries the second run id"), Second.RunId, SecondRunId);
	}
	TestTrue(TEXT("the first run's broadcast result was not overwritten"),
		Events.EndedResults[0].RunId == FirstRunId && Events.EndedResults[0].bCleared);
	return true;
}

#endif
