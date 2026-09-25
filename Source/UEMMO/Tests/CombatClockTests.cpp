#include "Misc/AutomationTest.h"

#include "../Combat/CombatClock.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// Acceptance tests for the fixed 60 Hz combat action clock.
// One logic step is 1/60 s; a single Advance runs at most 8 steps and keeps
// unconsumed backlog for later calls; frozen frames drop their delta; negative
// and non-finite deltas are ignored; Reset clears the backlog.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_006FrameRateTotals,
	"UEMMO.Tasks.M1_006.FrameRateTotals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_006FrameRateTotals::RunTest(const FString& Parameters)
{
	// One second of deltas at 30 fps must advance 60 steps in total.
	FCombatClock Clock30;
	int32 Total30 = 0;
	const double Delta30 = 1.0 / 30.0;
	for (int32 Index = 0; Index < 30; ++Index)
	{
		Total30 += Clock30.Advance(Delta30, false);
	}
	TestEqual(TEXT("30 deltas of 1/30 s advance 60 steps in total"), Total30, 60);

	// One second of deltas at 60 fps must advance 60 steps in total.
	FCombatClock Clock60;
	int32 Total60 = 0;
	const double Delta60 = 1.0 / 60.0;
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Total60 += Clock60.Advance(Delta60, false);
	}
	TestEqual(TEXT("60 deltas of 1/60 s advance 60 steps in total"), Total60, 60);

	// One second of deltas at 120 fps must advance 60 steps in total.
	FCombatClock Clock120;
	int32 Total120 = 0;
	const double Delta120 = 1.0 / 120.0;
	for (int32 Index = 0; Index < 120; ++Index)
	{
		Total120 += Clock120.Advance(Delta120, false);
	}
	TestEqual(TEXT("120 deltas of 1/120 s advance 60 steps in total"), Total120, 60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_006CapAndDrain,
	"UEMMO.Tasks.M1_006.CapAndDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_006CapAndDrain::RunTest(const FString& Parameters)
{
	// 0.5 s is 30 steps at 60 Hz, but a single Advance runs at most 8 steps.
	FCombatClock Clock;
	TestEqual(TEXT("Advance(0.5) is capped at 8 steps"), Clock.Advance(0.5, false), 8);

	// Zero-delta calls drain the remaining backlog 8 steps at a time; the
	// backlog must survive until fully consumed, so the total is exactly 30.
	int32 Total = 8;
	for (int32 Call = 0; Call < 5; ++Call)
	{
		const int32 Steps = Clock.Advance(0.0, false);
		Total += Steps;
		if (Steps == 0)
		{
			break;
		}
	}
	TestEqual(TEXT("draining the backlog after a capped frame yields exactly 30 steps"), Total, 30);
	TestEqual(TEXT("backlog is empty after draining"), Clock.Advance(0.0, false), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_006FrozenDelta,
	"UEMMO.Tasks.M1_006.FrozenDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_006FrozenDelta::RunTest(const FString& Parameters)
{
	// A frozen frame reports 0 steps and its delta is discarded outright.
	FCombatClock Clock;
	TestEqual(TEXT("frozen Advance returns 0 steps"), Clock.Advance(0.1, true), 0);
	TestEqual(TEXT("frozen delta is not accumulated"), Clock.Advance(0.0, false), 0);

	// The first normal frame after the freeze runs its own steps only:
	// there is no catch-up for the frozen 0.1 s.
	TestEqual(TEXT("first frame after unfreeze advances exactly 1 step"), Clock.Advance(1.0 / 60.0, false), 1);

	// A frozen frame must not drain existing backlog either: it returns 0
	// and leaves the backlog untouched for later normal calls.
	FCombatClock BacklogClock;
	TestEqual(TEXT("long frame is capped at 8 steps"), BacklogClock.Advance(0.5, false), 8);
	TestEqual(TEXT("frozen frame does not drain the backlog"), BacklogClock.Advance(0.2, true), 0);
	TestEqual(TEXT("backlog survives the freeze"), BacklogClock.Advance(0.0, false), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_006InvalidDeltas,
	"UEMMO.Tasks.M1_006.InvalidDeltas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_006InvalidDeltas::RunTest(const FString& Parameters)
{
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();

	FCombatClock Clock;
	TestEqual(TEXT("negative delta returns 0 steps"), Clock.Advance(-1.0 / 60.0, false), 0);
	TestEqual(TEXT("NaN delta returns 0 steps"), Clock.Advance(NaN, false), 0);
	TestEqual(TEXT("positive infinity delta returns 0 steps"), Clock.Advance(Inf, false), 0);
	TestEqual(TEXT("invalid deltas accumulate nothing"), Clock.Advance(0.0, false), 0);
	TestEqual(TEXT("normal advance is unaffected after invalid deltas"), Clock.Advance(1.0 / 60.0, false), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_006ResetClearsBacklog,
	"UEMMO.Tasks.M1_006.ResetClearsBacklog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_006ResetClearsBacklog::RunTest(const FString& Parameters)
{
	// Drain half of a long frame, then Reset must clear the leftover backlog.
	FCombatClock Clock;
	TestEqual(TEXT("half-drained long frame is capped at 8 steps"), Clock.Advance(0.5, false), 8);
	Clock.Reset();
	TestEqual(TEXT("Reset clears the leftover backlog"), Clock.Advance(0.0, false), 0);

	// After Reset the clock starts fresh: the next 0.5 s frame is capped at
	// 8 steps and its full backlog is exactly 30 steps, not 30 plus residue.
	TestEqual(TEXT("first frame after Reset is capped at 8 steps"), Clock.Advance(0.5, false), 8);
	int32 Total = 8;
	for (int32 Call = 0; Call < 5; ++Call)
	{
		const int32 Steps = Clock.Advance(0.0, false);
		Total += Steps;
		if (Steps == 0)
		{
			break;
		}
	}
	TestEqual(TEXT("post-Reset backlog totals exactly 30 steps, no residue carried over"), Total, 30);
	return true;
}

#endif
