// M5-072: the pure per-run scene-rule evaluator. The suite runs the
// evaluator entirely in memory - no world, no map load, no session - with a
// recording fake as the injected session adapter, locking:
//
//   1. Replay of a seen sequence is an idempotent no-op; reentrant submits
//      (a handler submitting during dispatch) are refused by name; commands
//      rejected by the adapter leave the once-rule armed (no fake
//      completion); a rejected terminal after a terminal keeps
//      first-terminal-wins (the adapter decides, the evaluator records).
//   2. Delayed commands ride the injected clock only: freeze before the due
//      time, fire at/after it, never on rewind or non-finite injection;
//      CancelRun drops the whole scope (stale dues never fire); ResetForRun
//      starts a fresh scope where the same rules may complete again; a
//      rejected delayed command retries honestly on the next matching event.
//
// The rules under test are the real production set: the M5-071 legacy
// migration output of a 2+3 wave room (enter -> wave 0, 1 s -> wave 1,
// wave 1 clears, death fails).

#include "Misc/AutomationTest.h"

#include "../Room/RoomDefinition.h"
#include "../Room/SceneRuleEvaluator.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_072
{
	/** The 2+3 wave list of the production combat room (shape only). */
	static TArray<FRoomWaveDefinition> M5_072_MakeTwoThreeWaves()
	{
		TArray<FRoomWaveDefinition> Waves;
		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = TEXT("melee_grunt");
		Wave0.Count = 2;
		Wave0.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
		Wave0.SpawnLocations.Add(FVector(450.0, -150.0, 88.0));
		Waves.Add(Wave0);
		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = TEXT("melee_grunt");
		Wave1.Count = 3;
		Wave1.SpawnLocations.Add(FVector(600.0, 100.0, 88.0));
		Wave1.SpawnLocations.Add(FVector(750.0, -100.0, 88.0));
		Wave1.SpawnLocations.Add(FVector(0.0, 300.0, 88.0));
		Waves.Add(Wave1);
		return Waves;
	}

	/** One recorded command of the fake session adapter. */
	struct FM5_072_RecordedCommand
	{
		FName RuleId;
		ERoomRuleEffect Effect;
		int32 WaveIndex;
	};

	/**
	 * The fake session adapter: records every command and answers like the
	 * real session's guards - StartWave is accepted; the FIRST terminal
	 * (MarkCleared / FailRun) is accepted and every later terminal is
	 * rejected with the first-terminal-wins reason; optional scripted
	 * rejections let the tests exercise the no-fake-completion path.
	 */
	struct FM5_072_FakeAdapter
	{
		TArray<FM5_072_RecordedCommand> Commands;
		bool bTerminalDone = false;
		FName ForceRejectRule;

		FRoomRuleDecision operator()(const FRoomRuleCommand& Command)
		{
			Commands.Add({ Command.RuleId, Command.Effect, Command.WaveIndex });
			if (ForceRejectRule == Command.RuleId)
			{
				return { ERoomRuleDecisionType::Rejected,
					FString::Printf(TEXT("session refused '%s' (scripted rejection)"), *Command.RuleId.ToString()) };
			}
			if (Command.Effect == ERoomRuleEffect::MarkCleared || Command.Effect == ERoomRuleEffect::FailRun)
			{
				if (bTerminalDone)
				{
					return { ERoomRuleDecisionType::Rejected, TEXT("first terminal state wins - the outcome is kept") };
				}
				bTerminalDone = true;
			}
			return { ERoomRuleDecisionType::Accepted, TEXT("applied") };
		}

		int32 CountOf(ERoomRuleEffect Effect) const
		{
			int32 Count = 0;
			for (const FM5_072_RecordedCommand& Command : Commands)
			{
				if (Command.Effect == Effect)
				{
					++Count;
				}
			}
			return Count;
		}
	};

	/** A minimal event builder. */
	static FRoomRuleEvent M5_072_MakeEvent(uint64 RunId, uint64 Sequence, ERoomRuleEvent Type,
		int32 WaveIndex = -1, FName RegionId = FName())
	{
		FRoomRuleEvent Event;
		Event.RunId = RunId;
		Event.RoomId = TEXT("room_combat_01");
		Event.EventSequence = Sequence;
		Event.Type = Type;
		Event.RegionId = RegionId;
		Event.WaveIndex = WaveIndex;
		return Event;
	}
}

using namespace UE::UEMMO::Tasks::M5_072;

// 1. Acceptance: replay idempotence, reentrancy refusal, no fake completion
//    on a rejected command, and first-terminal-wins through the adapter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_072ReplayReentrancyRejectionAndTerminalFirstWins,
	"UEMMO.Tasks.M5_072.ReplayReentrancyRejectionAndTerminalFirstWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_072ReplayReentrancyRejectionAndTerminalFirstWins::RunTest(const FString& Parameters)
{
	FM5_072_FakeAdapter Adapter;
	FSceneRuleEvaluator Evaluator;

	TArray<FRoomSceneRule> Rules;
	TestTrue(TEXT("the production rules migrate from the 2+3 waves"),
		BuildLegacySchemaOneDefaultRules(M5_072_MakeTwoThreeWaves(), Rules));
	Evaluator.ResetForRun(/*RunId*/ 7, TEXT("room_combat_01"), Rules,
		[&Adapter](const FRoomRuleCommand& Command) { return Adapter(Command); });
	TestEqual(TEXT("four rules are armed"), Evaluator.GetRuleCount(), 4);

	// Entry event: StartWave(0) immediately (delay 0).
	FRoomRuleDecision EnterDecision = Evaluator.SubmitEvent(
		M5_072_MakeEvent(7, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TestEqual(TEXT("the entry command is accepted"), static_cast<int32>(EnterDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::Accepted));
	TestEqual(TEXT("exactly one command reached the adapter"), Adapter.Commands.Num(), 1);
	TestEqual(TEXT("the entry command starts wave 0"), Adapter.Commands[0].WaveIndex, 0);
	TestTrue(TEXT("the entry once-rule completed"), Evaluator.IsRuleCompleted(TEXT("enter_start_wave_0")));

	// Replay of the same sequence: idempotent no-op.
	FRoomRuleDecision ReplayDecision = Evaluator.SubmitEvent(
		M5_072_MakeEvent(7, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TestEqual(TEXT("the replay returns AlreadyApplied"), static_cast<int32>(ReplayDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::AlreadyApplied));
	TestEqual(TEXT("the replay fired no second command"), Adapter.Commands.Num(), 1);
	TestTrue(TEXT("the replay reason names the sequence"), ReplayDecision.Reason.Contains(TEXT("replay of sequence 1")));

	// Sequence rewind is a replay too.
	FRoomRuleDecision RewindDecision = Evaluator.SubmitEvent(
		M5_072_MakeEvent(7, 1, ERoomRuleEvent::WaveCleared, 0));
	TestEqual(TEXT("a sequence rewind is refused as a replay"), static_cast<int32>(RewindDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::AlreadyApplied));
	TestEqual(TEXT("the rewind fired nothing"), Adapter.Commands.Num(), 1);

	// Old-run event: refused outright (its dues and rules belong to another
	// run; the evaluator must never act on them).
	FRoomRuleDecision StaleDecision = Evaluator.SubmitEvent(
		M5_072_MakeEvent(/*RunId*/ 6, 2, ERoomRuleEvent::PlayerDied));
	TestEqual(TEXT("an old-run event is rejected"), static_cast<int32>(StaleDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::Rejected));
	TestTrue(TEXT("the rejection names the foreign run"), StaleDecision.Reason.Contains(TEXT("foreign run")));

	// Reentrancy: the adapter submits back during dispatch.
	FM5_072_FakeAdapter ReentrantAdapter;
	FSceneRuleEvaluator ReentrantEvaluator;
	ReentrantEvaluator.ResetForRun(/*RunId*/ 8, TEXT("room_combat_01"), Rules,
		[this, &ReentrantAdapter, &ReentrantEvaluator](const FRoomRuleCommand& Command)
		{
			FRoomRuleDecision Inner = ReentrantEvaluator.SubmitEvent(
				M5_072_MakeEvent(8, 99, ERoomRuleEvent::PlayerDied));
			TestEqual(TEXT("the nested submit is refused by name"), static_cast<int32>(Inner.Type),
				static_cast<int32>(ERoomRuleDecisionType::Rejected));
			TestTrue(TEXT("the nested refusal names reentrancy"), Inner.Reason.Contains(TEXT("reentrant")));
			return ReentrantAdapter(Command);
		});
	ReentrantEvaluator.SubmitEvent(M5_072_MakeEvent(8, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TestEqual(TEXT("the reentrant run dispatched its own command exactly once"),
		ReentrantAdapter.Commands.Num(), 1);

	// A rejected command leaves the once-rule armed (no fake completion) and
	// a later matching event retries honestly.
	FM5_072_FakeAdapter RejectingAdapter;
	RejectingAdapter.ForceRejectRule = TEXT("enter_start_wave_0");
	FSceneRuleEvaluator RetryEvaluator;
	RetryEvaluator.ResetForRun(/*RunId*/ 9, TEXT("room_combat_01"), Rules,
		[&RejectingAdapter](const FRoomRuleCommand& Command) { return RejectingAdapter(Command); });
	FRoomRuleDecision RejectedDecision = RetryEvaluator.SubmitEvent(
		M5_072_MakeEvent(9, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TestEqual(TEXT("the scripted rejection reaches the caller"), static_cast<int32>(RejectedDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::Rejected));
	TestFalse(TEXT("the rejected rule did not complete"), RetryEvaluator.IsRuleCompleted(TEXT("enter_start_wave_0")));
	TestEqual(TEXT("the completed count stays zero"), RetryEvaluator.GetCompletedRuleCount(), 0);
	RejectingAdapter.ForceRejectRule = FName();
	FRoomRuleDecision RetryDecision = RetryEvaluator.SubmitEvent(
		M5_072_MakeEvent(9, 2, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TestEqual(TEXT("the retried entry succeeds"), static_cast<int32>(RetryDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::Accepted));
	TestTrue(TEXT("the retried rule completes now"), RetryEvaluator.IsRuleCompleted(TEXT("enter_start_wave_0")));

	// First terminal wins through the adapter: after MarkCleared, the death
	// rule's FailRun is rejected and recorded - never forced through.
	FM5_072_FakeAdapter TerminalAdapter;
	FSceneRuleEvaluator TerminalEvaluator;
	TerminalEvaluator.ResetForRun(/*RunId*/ 10, TEXT("room_combat_01"), Rules,
		[&TerminalAdapter](const FRoomRuleCommand& Command) { return TerminalAdapter(Command); });
	TerminalEvaluator.SubmitEvent(M5_072_MakeEvent(10, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TerminalEvaluator.AdvanceClock(1.0);
	TerminalEvaluator.SubmitEvent(M5_072_MakeEvent(10, 2, ERoomRuleEvent::WaveCleared, 0));
	TerminalEvaluator.AdvanceClock(3.0);
	FRoomRuleDecision ClearDecision = TerminalEvaluator.SubmitEvent(
		M5_072_MakeEvent(10, 3, ERoomRuleEvent::WaveCleared, 1));
	TestEqual(TEXT("wave 1's clear is accepted"), static_cast<int32>(ClearDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::Accepted));
	FRoomRuleDecision DeathDecision = TerminalEvaluator.SubmitEvent(
		M5_072_MakeEvent(10, 4, ERoomRuleEvent::PlayerDied));
	TestEqual(TEXT("the death rule after the terminal is rejected"), static_cast<int32>(DeathDecision.Type),
		static_cast<int32>(ERoomRuleDecisionType::Rejected));
	TestTrue(TEXT("the rejection carries the first-terminal reason"),
		DeathDecision.Reason.Contains(TEXT("first terminal state wins")));
	TestFalse(TEXT("the rejected death rule stays armed"),
		TerminalEvaluator.IsRuleCompleted(TEXT("player_dead_fail")));
	TestTrue(TEXT("exactly one terminal was applied (the adapter's first-terminal flag)"),
		TerminalAdapter.bTerminalDone);
	return true;
}

// 2. Acceptance: delayed commands ride the injected clock only - frozen
//    before due, fired at/after due, never on rewind or non-finite input;
//    CancelRun kills the scope; ResetForRun re-arms the same rules; the
//    evaluator clock never reads a wall clock.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_072DelayedCommandsFollowTheInjectedClock,
	"UEMMO.Tasks.M5_072.DelayedCommandsFollowTheInjectedClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_072DelayedCommandsFollowTheInjectedClock::RunTest(const FString& Parameters)
{
	FM5_072_FakeAdapter Adapter;
	FSceneRuleEvaluator Evaluator;
	TArray<FRoomSceneRule> Rules;
	BuildLegacySchemaOneDefaultRules(M5_072_MakeTwoThreeWaves(), Rules);
	Evaluator.ResetForRun(/*RunId*/ 1, TEXT("room_combat_01"), Rules,
		[&Adapter](const FRoomRuleCommand& Command) { return Adapter(Command); });

	// Entry at clock 0, wave 0 cleared at clock 0.5: the 1 s delay arms a
	// pending command due at 1.5.
	Evaluator.AdvanceClock(0.0);
	Evaluator.SubmitEvent(M5_072_MakeEvent(1, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	Evaluator.AdvanceClock(0.5);
	Evaluator.SubmitEvent(M5_072_MakeEvent(1, 2, ERoomRuleEvent::WaveCleared, 0));
	TestEqual(TEXT("the delayed wave-1 start is armed"), Evaluator.GetPendingDelayedCommandCount(), 1);
	TestEqual(TEXT("no command fired yet"), Adapter.Commands.Num(), 1);

	// Freeze before due: nothing fires.
	TArray<FRoomRuleDecision> Early = Evaluator.AdvanceClock(1.4);
	TestEqual(TEXT("the pre-due advance fired nothing"), Early.Num(), 0);
	TestEqual(TEXT("the pending command stays armed"), Evaluator.GetPendingDelayedCommandCount(), 1);

	// Non-finite injection: ignored entirely (clock keeps its value).
	TArray<FRoomRuleDecision> NanAdvance = Evaluator.AdvanceClock(std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("a non-finite advance fires nothing"), NanAdvance.Num(), 0);
	TestEqual(TEXT("the clock ignored the non-finite value"), Evaluator.GetClockSeconds(), 1.4);

	// Rewind: the older (larger) clock stays, nothing re-fires.
	TArray<FRoomRuleDecision> Rewind = Evaluator.AdvanceClock(0.2);
	TestEqual(TEXT("a rewound advance fires nothing"), Rewind.Num(), 0);
	TestEqual(TEXT("the clock did not rewind"), Evaluator.GetClockSeconds(), 1.4);

	// At due: exactly one StartWave(1) command fires and completes its rule.
	TArray<FRoomRuleDecision> Due = Evaluator.AdvanceClock(1.5);
	TestEqual(TEXT("the due advance dispatched one command"), Due.Num(), 1);
	TestEqual(TEXT("the due decision is accepted"), static_cast<int32>(Due[0].Type),
		static_cast<int32>(ERoomRuleDecisionType::Accepted));
	TestEqual(TEXT("the delayed command started wave 1"), Adapter.Commands.Last().WaveIndex, 1);
	TestTrue(TEXT("the delayed rule completed"), Evaluator.IsRuleCompleted(TEXT("wave_0_start_wave_1")));
	TestEqual(TEXT("no pending command remains"), Evaluator.GetPendingDelayedCommandCount(), 0);

	// Re-sending the same cleared event (new sequence) must not re-arm the
	// completed once-rule.
	Evaluator.SubmitEvent(M5_072_MakeEvent(1, 3, ERoomRuleEvent::WaveCleared, 0));
	TestEqual(TEXT("the completed once-rule does not re-arm"), Evaluator.GetPendingDelayedCommandCount(), 0);

	// CancelRun with a foreign id changes nothing; the matching id kills the
	// whole scope - a far-future clock advance can never fire stale dues.
	Evaluator.CancelRun(/*RunId*/ 42);
	TestEqual(TEXT("a foreign CancelRun keeps the run active"), Evaluator.GetRunId(), static_cast<uint64>(1));
	Evaluator.AdvanceClock(2.0);
	Evaluator.SubmitEvent(M5_072_MakeEvent(1, 4, ERoomRuleEvent::WaveCleared, 1));
	Evaluator.CancelRun(/*RunId*/ 1);
	TestEqual(TEXT("the matching CancelRun drops the run scope"), Evaluator.GetRunId(), static_cast<uint64>(0));
	const int32 CommandsAtCancel = Adapter.Commands.Num();
	TArray<FRoomRuleDecision> AfterCancel = Evaluator.AdvanceClock(1000.0);
	TestEqual(TEXT("no stale due fires after the cancel"), AfterCancel.Num(), 0);
	TestEqual(TEXT("no command reached the adapter after the cancel"), Adapter.Commands.Num(), CommandsAtCancel);
	FRoomRuleDecision PostCancelEvent = Evaluator.SubmitEvent(M5_072_MakeEvent(1, 5, ERoomRuleEvent::PlayerDied));
	TestEqual(TEXT("a post-cancel event is rejected"), static_cast<int32>(PostCancelEvent.Type),
		static_cast<int32>(ERoomRuleDecisionType::Rejected));

	// ResetForRun: the same rules complete again for the fresh run, and the
	// sequence watermark starts over at 1.
	FM5_072_FakeAdapter SecondAdapter;
	FSceneRuleEvaluator SecondScope;
	SecondScope.ResetForRun(/*RunId*/ 2, TEXT("room_combat_01"), Rules,
		[&SecondAdapter](const FRoomRuleCommand& Command) { return SecondAdapter(Command); });
	FRoomRuleDecision FreshEntry = SecondScope.SubmitEvent(
		M5_072_MakeEvent(2, 1, ERoomRuleEvent::PlayerEnteredRegion, -1, TEXT("activation")));
	TestEqual(TEXT("the fresh run's entry is accepted"), static_cast<int32>(FreshEntry.Type),
		static_cast<int32>(ERoomRuleDecisionType::Accepted));
	TestTrue(TEXT("the fresh run completed the entry rule"),
		SecondScope.IsRuleCompleted(TEXT("enter_start_wave_0")));
	return true;
}

#endif
