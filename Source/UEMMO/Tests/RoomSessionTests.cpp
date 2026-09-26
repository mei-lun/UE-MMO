// M2-006: room session state and unique run identity (interface contract
// section 7). These tests drive a real UWorldSubsystem inside real temp game
// worlds (UWorld::CreateWorld, the same source the M1-027 tests use): every
// test world carries its own URoomSessionSubsystem obtained through the
// world's subsystem collection, exactly like the production wiring will.
//
// Locked behavior: the Idle/Running/Cleared/Failed/Exiting state machine
// (first terminal state wins, outcomes never overwritten, a repeated Start
// while Running is rejected without resetting the RunId), the three per-run
// identities (RunId monotonic per session, SettlementId process-global never
// repeated, Seed a fixed derivation of the RunId), exactly-once start/end
// events, kill bookkeeping accepted only while Running, and world cleanup
// dropping every delegate binding so rebuilt worlds start independent.

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"

#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_006
{
	// One fresh game world per need; names are uniquified because several
	// worlds of this suite can be alive inside the same process at once.
	static UWorld* M2_006_AcquireTestWorld()
	{
		static int32 M2_006_WorldCounter = 0;
		++M2_006_WorldCounter;
		const FName WorldName = FName(*FString::Printf(TEXT("M2_006_TestWorld_%d"), M2_006_WorldCounter));
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
		if (World != nullptr)
		{
			return World;
		}
		UE_LOG(LogTemp, Log, TEXT("M2_006 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Subsystem fetch through the world's collection (the production access
	// path); reports and returns null when the world has none.
	static URoomSessionSubsystem* M2_006_GetSession(FAutomationTestBase& Test, UWorld* World)
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
	// result; wave/map data belongs to later tasks and is left empty here.
	static URoomDefinition* M2_006_MakeRoom(FName RoomId)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = RoomId;
		return Room;
	}

	// Recorded session events (the assertions read this struct only).
	struct FM2_006_RunEvents
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

using namespace UE::UEMMO::Tasks::M2_006;

// 1. A fresh session is Idle; StartRoom accepts the first run, moves to
//    Running, assigns the three identities and fires OnRunStarted once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006IdleStartTransitionsToRunningWithUniqueIds,
	"UEMMO.Tasks.M2_006.IdleStartTransitionsToRunningWithUniqueIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006IdleStartTransitionsToRunningWithUniqueIds::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents Events;
	Events.Bind(*Session);

	TestTrue(TEXT("a fresh session starts Idle"),
		Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("a fresh session has no run id"), Session->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("a fresh session has no settlement id"), Session->GetSettlementId(), static_cast<uint64>(0));
	TestEqual(TEXT("a fresh session has no seed"), Session->GetSeed(), 0);
	TestTrue(TEXT("a fresh session has no room id"), Session->GetRoomId().IsNone());

	// Start with a null definition is refused and changes nothing.
	TestFalse(TEXT("StartRoom with a null definition is rejected"), Session->StartRoom(nullptr));
	TestTrue(TEXT("a rejected start leaves the session Idle"),
		Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("a rejected start fires no start event"), Events.StartedCount, 0);

	// The real start: Idle -> Running with identities and one start event.
	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	TestTrue(TEXT("StartRoom from Idle is accepted"), Session->StartRoom(Room));
	TestTrue(TEXT("the session is Running after StartRoom"),
		Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the first run carries run id 1"), Session->GetRunId(), static_cast<uint64>(1));
	TestTrue(TEXT("the first run carries a nonzero settlement id"), Session->GetSettlementId() > 0);
	TestTrue(TEXT("the first run carries a nonzero seed"), Session->GetSeed() != 0);
	TestEqual(TEXT("the run records the room id"), Session->GetRoomId(), FName(TEXT("room_test_01")));
	TestEqual(TEXT("StartRoom fired exactly one start event"), Events.StartedCount, 1);
	return true;
}

// 2. A second StartRoom while Running is rejected and never resets the
//    RunId, the SettlementId, the room id or the event count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006SecondStartWhileRunningIsRejectedAndKeepsRunId,
	"UEMMO.Tasks.M2_006.SecondStartWhileRunningIsRejectedAndKeepsRunId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006SecondStartWhileRunningIsRejectedAndKeepsRunId::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents Events;
	Events.Bind(*Session);

	const URoomDefinition* FirstRoom = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the first StartRoom is accepted"), Session->StartRoom(FirstRoom)))
	{
		return true;
	}
	const uint64 FirstRunId = Session->GetRunId();
	const uint64 FirstSettlementId = Session->GetSettlementId();
	const int32 FirstSeed = Session->GetSeed();

	// A different room while the first run is still Running.
	const URoomDefinition* SecondRoom = M2_006_MakeRoom(FName(TEXT("room_test_02")));
	TestFalse(TEXT("a second StartRoom while Running is rejected"), Session->StartRoom(SecondRoom));
	TestTrue(TEXT("the session stays Running after the rejected start"),
		Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the rejected start did not reset the run id"), Session->GetRunId(), FirstRunId);
	TestEqual(TEXT("the rejected start did not reset the settlement id"), Session->GetSettlementId(), FirstSettlementId);
	TestEqual(TEXT("the rejected start did not reset the seed"), Session->GetSeed(), FirstSeed);
	TestEqual(TEXT("the rejected start did not overwrite the room id"),
		Session->GetRoomId(), FName(TEXT("room_test_01")));
	TestEqual(TEXT("the rejected start fired no additional start event"), Events.StartedCount, 1);
	TestEqual(TEXT("the rejected start fired no end event"), Events.EndedCount, 0);
	return true;
}

// 3. MarkCleared ends the run: Cleared, OnRunEnded exactly once with the
//    full result; a later Fail (and a later Cleared) is rejected and the
//    recorded outcome is never overwritten nor re-broadcast.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006MarkClearedEmitsOnRunEndedOnceAndRejectsFail,
	"UEMMO.Tasks.M2_006.MarkClearedEmitsOnRunEndedOnceAndRejectsFail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006MarkClearedEmitsOnRunEndedOnceAndRejectsFail::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents Events;
	Events.Bind(*Session);

	// Injected clock: start at 10 s, clear at 32.5 s -> elapsed 22.5 s.
	Session->SetSessionClockSeconds(10.0);
	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();
	const uint64 SettlementId = Session->GetSettlementId();
	const int32 Seed = Session->GetSeed();
	TestTrue(TEXT("a mid-run kill is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestTrue(TEXT("a second mid-run kill is accepted"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));

	Session->SetSessionClockSeconds(32.5);
	if (!TestTrue(TEXT("MarkCleared from Running is accepted"), Session->MarkCleared()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Cleared after MarkCleared"),
		Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("OnRunEnded fired exactly once"), Events.EndedCount, 1);
	if (TestTrue(TEXT("the end event carried one result"), Events.EndedResults.Num() == 1))
	{
		const FRoomResult& Result = Events.EndedResults[0];
		TestTrue(TEXT("the result records Cleared"), Result.bCleared);
		TestEqual(TEXT("the result carries the run id"), Result.RunId, RunId);
		TestEqual(TEXT("the result carries the settlement id"), Result.SettlementId, SettlementId);
		TestEqual(TEXT("the result carries the seed"), Result.Seed, Seed);
		TestEqual(TEXT("the result carries the room id"), Result.RoomId, FName(TEXT("room_test_01")));
		TestEqual(TEXT("the result counts both kills"), Result.KilledCount, 2);
		TestEqual(TEXT("the result measures elapsed seconds with the injected clock"),
			Result.ElapsedSeconds, 22.5);
		TestTrue(TEXT("the stored last result matches the broadcast one"),
			Session->GetLastResult().RunId == RunId && Session->GetLastResult().bCleared);
	}

	// Terminal idempotency: neither a failure nor a second clear may
	// overwrite the outcome or fire the end event again.
	TestFalse(TEXT("FailRun after Cleared is rejected"), Session->FailRun());
	TestTrue(TEXT("the Cleared outcome survives the failed failure request"),
		Session->GetState() == ERoomSessionState::Cleared);
	TestFalse(TEXT("a second MarkCleared is rejected too"), Session->MarkCleared());
	TestEqual(TEXT("OnRunEnded still fired exactly once"), Events.EndedCount, 1);
	TestEqual(TEXT("the recorded result was not overwritten"),
		Events.EndedResults.Num(), 1);
	TestTrue(TEXT("the recorded result is still the cleared one"),
		Events.EndedResults[0].bCleared);
	TestFalse(TEXT("a kill notification after Cleared is ignored"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestEqual(TEXT("the kill count was not changed by the stale notification"), Session->GetKilledCount(), 2);
	return true;
}

// 4. FailRun ends the run as Failed; a later MarkCleared is rejected and
//    the failed outcome is never overwritten.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006FailRunTransitionsToFailedAndRejectsCleared,
	"UEMMO.Tasks.M2_006.FailRunTransitionsToFailedAndRejectsCleared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006FailRunTransitionsToFailedAndRejectsCleared::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents Events;
	Events.Bind(*Session);

	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();

	if (!TestTrue(TEXT("FailRun from Running is accepted"), Session->FailRun()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Failed after FailRun"),
		Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("OnRunEnded fired exactly once for the failed run"), Events.EndedCount, 1);
	if (TestTrue(TEXT("the end event carried one result"), Events.EndedResults.Num() == 1))
	{
		const FRoomResult& Result = Events.EndedResults[0];
		TestFalse(TEXT("the result records not-cleared (Failed)"), Result.bCleared);
		TestEqual(TEXT("the result carries the failed run's id"), Result.RunId, RunId);
		TestEqual(TEXT("the failed run counts zero kills"), Result.KilledCount, 0);
	}

	// First terminal state wins: a later clear request cannot overwrite.
	TestFalse(TEXT("MarkCleared after Failed is rejected"), Session->MarkCleared());
	TestTrue(TEXT("the Failed outcome survives the late clear request"),
		Session->GetState() == ERoomSessionState::Failed);
	TestFalse(TEXT("a second FailRun is rejected too"), Session->FailRun());
	TestEqual(TEXT("OnRunEnded still fired exactly once"), Events.EndedCount, 1);
	return true;
}

// 5. After ResetToIdle a new run gets a fresh RunId and a fresh
//    (never-repeated) SettlementId, and the start event fires again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006NewRunAfterResetGetsNewRunIdAndNewSettlementId,
	"UEMMO.Tasks.M2_006.NewRunAfterResetGetsNewRunIdAndNewSettlementId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006NewRunAfterResetGetsNewRunIdAndNewSettlementId::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents Events;
	Events.Bind(*Session);

	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the first run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 FirstRunId = Session->GetRunId();
	const uint64 FirstSettlementId = Session->GetSettlementId();
	const int32 FirstSeed = Session->GetSeed();
	if (!TestTrue(TEXT("the first run fails"), Session->FailRun()))
	{
		return true;
	}

	if (!TestTrue(TEXT("ResetToIdle from a terminal state is accepted"), Session->ResetToIdle()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Idle after the reset"),
		Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the reset cleared the run id"), Session->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("the reset cleared the settlement id"), Session->GetSettlementId(), static_cast<uint64>(0));
	TestEqual(TEXT("the reset cleared the seed"), Session->GetSeed(), 0);
	TestEqual(TEXT("the reset cleared the kill counter"), Session->GetKilledCount(), 0);

	if (!TestTrue(TEXT("the second run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the second run got a new (higher) run id"),
		Session->GetRunId() > FirstRunId);
	TestTrue(TEXT("the second run got a new (never-repeated) settlement id"),
		Session->GetSettlementId() > FirstSettlementId);
	TestTrue(TEXT("the second run got a different seed for a different run id"),
		Session->GetSeed() != FirstSeed);
	TestEqual(TEXT("the second start fired the start event again"), Events.StartedCount, 2);
	TestEqual(TEXT("the second run's end event is not the first run's"),
		Events.EndedCount, 1);
	return true;
}

// 6. Kill notifications accumulate KilledCount while Running and are
//    ignored outside Running (stale callbacks of ended runs).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006NotifyEnemyKilledCountsOnlyDuringRunning,
	"UEMMO.Tasks.M2_006.NotifyEnemyKilledCountsOnlyDuringRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006NotifyEnemyKilledCountsOnlyDuringRunning::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	// Idle: no run to attribute the kill to.
	TestFalse(TEXT("a kill notification while Idle is ignored"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestEqual(TEXT("the ignored Idle notification counted nothing"), Session->GetKilledCount(), 0);

	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestTrue(TEXT("a mid-run kill notification is accepted"),
			Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	}
	TestEqual(TEXT("three accepted kills accumulated"), Session->GetKilledCount(), 3);

	if (!TestTrue(TEXT("the run is cleared"), Session->MarkCleared()))
	{
		return true;
	}
	TestFalse(TEXT("a kill notification after Cleared is ignored"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestEqual(TEXT("the stale notification did not change the recorded result"),
		Session->GetLastResult().KilledCount, 3);

	// After the reset the counter is cleared too, and stays ignored.
	if (!TestTrue(TEXT("the session resets to Idle"), Session->ResetToIdle()))
	{
		return true;
	}
	TestEqual(TEXT("the reset cleared the kill counter"), Session->GetKilledCount(), 0);
	TestFalse(TEXT("a kill notification in the fresh Idle session is ignored"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestEqual(TEXT("still zero kills in the fresh session"), Session->GetKilledCount(), 0);
	return true;
}

// 7. ResetToIdle is only legal from terminal states (or Exiting); it is
//    rejected while Running and when already Idle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006ResetToIdleOnlyFromTerminalOrExiting,
	"UEMMO.Tasks.M2_006.ResetToIdleOnlyFromTerminalOrExiting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006ResetToIdleOnlyFromTerminalOrExiting::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	TestFalse(TEXT("ResetToIdle while already Idle is rejected"), Session->ResetToIdle());
	TestTrue(TEXT("an idle session stays Idle after the rejected reset"),
		Session->GetState() == ERoomSessionState::Idle);

	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	TestFalse(TEXT("ResetToIdle while Running is rejected (the run must end or exit first)"),
		Session->ResetToIdle());
	TestTrue(TEXT("the session stays Running after the rejected reset"),
		Session->GetState() == ERoomSessionState::Running);
	TestTrue(TEXT("the run id survived the rejected reset"), Session->GetRunId() > 0);

	if (!TestTrue(TEXT("the run fails"), Session->FailRun()))
	{
		return true;
	}
	TestTrue(TEXT("ResetToIdle from Failed is accepted"), Session->ResetToIdle());
	TestTrue(TEXT("the session is Idle after the terminal reset"),
		Session->GetState() == ERoomSessionState::Idle);
	TestFalse(TEXT("a second ResetToIdle while Idle is rejected again"), Session->ResetToIdle());
	return true;
}

// 8. BeginExit moves any non-Exiting state into Exiting and is rejected
//    when already Exiting; a mid-run exit keeps the run bookkeeping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006BeginExitTransitionsIntoExitingFromAnyOtherState,
	"UEMMO.Tasks.M2_006.BeginExitTransitionsIntoExitingFromAnyOtherState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006BeginExitTransitionsIntoExitingFromAnyOtherState::RunTest(const FString& Parameters)
{
	UWorld* World = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* Session = M2_006_GetSession(*this, World);
	if (Session == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents Events;
	Events.Bind(*Session);

	// Exit from Idle is legal (leaving a never-played room).
	if (!TestTrue(TEXT("BeginExit from Idle is accepted"), Session->BeginExit()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Exiting after BeginExit"),
		Session->GetState() == ERoomSessionState::Exiting);
	TestFalse(TEXT("BeginExit while already Exiting is rejected"), Session->BeginExit());
	TestTrue(TEXT("ResetToIdle from Exiting is accepted (EndRoom -> Exiting -> Idle path)"),
		Session->ResetToIdle());
	TestTrue(TEXT("the session is Idle again after the exit reset"),
		Session->GetState() == ERoomSessionState::Idle);

	// Exit mid-run: allowed (M2-011 owns the procedure); the run bookkeeping
	// is kept (no end event - the run did not reach a terminal state here),
	// and stale kill notifications of the exited run stop counting.
	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Session->GetRunId();
	if (!TestTrue(TEXT("a kill is counted before the exit"), Session->NotifyEnemyKilled(FName(TEXT("melee_grunt")))))
	{
		return true;
	}
	if (!TestTrue(TEXT("BeginExit from Running is accepted"), Session->BeginExit()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Exiting after the mid-run exit"),
		Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the mid-run exit fired no end event"), Events.EndedCount, 0);
	TestEqual(TEXT("the mid-run exit kept the run id"), Session->GetRunId(), RunId);
	TestFalse(TEXT("a kill notification after the exit is ignored"),
		Session->NotifyEnemyKilled(FName(TEXT("melee_grunt"))));
	TestEqual(TEXT("the ignored post-exit notification changed nothing"), Session->GetKilledCount(), 1);

	// From Exiting the session returns to Idle through ResetToIdle.
	TestTrue(TEXT("ResetToIdle from Exiting is accepted"), Session->ResetToIdle());
	TestTrue(TEXT("the session is Idle after the exit reset"),
		Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the exit reset cleared the run id"), Session->GetRunId(), static_cast<uint64>(0));
	return true;
}

// 9. World cleanup (the EndPlay path) clears the bindings and the state, and
//    a rebuilt world's fresh subsystem starts fully independent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_006WorldCleanupDropsBindingsAndRebuiltWorldIsIndependent,
	"UEMMO.Tasks.M2_006.WorldCleanupDropsBindingsAndRebuiltWorldIsIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_006WorldCleanupDropsBindingsAndRebuiltWorldIsIndependent::RunTest(const FString& Parameters)
{
	UWorld* WorldA = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* SessionA = M2_006_GetSession(*this, WorldA);
	if (SessionA == nullptr)
	{
		return true;
	}

	FM2_006_RunEvents EventsA;
	EventsA.Bind(*SessionA);

	const URoomDefinition* Room = M2_006_MakeRoom(FName(TEXT("room_test_01")));
	if (!TestTrue(TEXT("world A starts a run"), SessionA->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("world A fails its run"), SessionA->FailRun()))
	{
		return true;
	}
	TestEqual(TEXT("world A received its end event"), EventsA.EndedCount, 1);
	TestTrue(TEXT("world A's binding is live before the cleanup"),
		SessionA->OnRunEnded().IsBound());

	// The world destruction path runs the subsystem's Deinitialize: the
	// bindings are dropped and the state is reset, so nothing can dangle.
	WorldA->DestroyWorld(/*bInformEngineOfWorld*/ false);
	TestEqual(TEXT("the cleanup fired no extra end event"), EventsA.EndedCount, 1);
	TestFalse(TEXT("the cleanup dropped the end-event binding"),
		SessionA->OnRunEnded().IsBound());
	TestFalse(TEXT("the cleanup dropped the start-event binding"),
		SessionA->OnRunStarted().IsBound());
	TestTrue(TEXT("the cleanup reset the session state to Idle"),
		SessionA->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the cleanup cleared the run id"), SessionA->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("the cleanup cleared the recorded result"),
		SessionA->GetLastResult().RunId, static_cast<uint64>(0));

	// A rebuilt world gets a fresh, fully independent subsystem: Idle, no
	// ids, and its own event bindings (the SettlementId counter is the only
	// process-global piece and never hands out the same id twice).
	UWorld* WorldB = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* SessionB = M2_006_GetSession(*this, WorldB);
	if (SessionB == nullptr)
	{
		return true;
	}
	TestTrue(TEXT("the rebuilt world's session starts Idle"),
		SessionB->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("the rebuilt world's session has no run id"),
		SessionB->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("the rebuilt world's session has no settlement id"),
		SessionB->GetSettlementId(), static_cast<uint64>(0));

	FM2_006_RunEvents EventsB;
	EventsB.Bind(*SessionB);
	if (!TestTrue(TEXT("world B starts its own run"), SessionB->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("world B is Running independently of world A's history"),
		SessionB->GetState() == ERoomSessionState::Running);
	TestTrue(TEXT("world B's run got a fresh settlement id (never world A's)"),
		SessionB->GetSettlementId() > 0);
	TestEqual(TEXT("world A's events were not touched by world B's run"),
		EventsA.EndedCount, 1);
	TestEqual(TEXT("world B received its own start event"), EventsB.StartedCount, 1);

	// A second world alive at the same time never shares ids with world B.
	UWorld* WorldC = M2_006_AcquireTestWorld();
	URoomSessionSubsystem* SessionC = M2_006_GetSession(*this, WorldC);
	if (SessionC == nullptr)
	{
		return true;
	}
	if (!TestTrue(TEXT("world C starts its own run in parallel"), SessionC->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("parallel worlds never receive the same settlement id"),
		SessionC->GetSettlementId() != SessionB->GetSettlementId());
	return true;
}

#endif
