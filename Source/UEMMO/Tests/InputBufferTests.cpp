#include "Misc/AutomationTest.h"

#include "../Combat/CombatInputBuffer.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_003
{
	FBufferedCombatInput MakeInput(uint64 Sequence, ECombatInput Action)
	{
		FBufferedCombatInput Input;
		Input.Sequence = Sequence;
		Input.Action = Action;
		Input.PressedAt = 0.0;
		return Input;
	}
}

using namespace UE::UEMMO::Tasks::M1_003;

namespace UE::UEMMO::Tasks::M1_004
{
	constexpr double TestLifetime = 0.150;

	FBufferedCombatInput MakeTimedInput(uint64 Sequence, ECombatInput Action, double PressedAt)
	{
		FBufferedCombatInput Input;
		Input.Sequence = Sequence;
		Input.Action = Action;
		Input.PressedAt = PressedAt;
		return Input;
	}
}

using namespace UE::UEMMO::Tasks::M1_004;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_003EmptyQueue,
	"UEMMO.Tasks.M1_003.EmptyQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_003EmptyQueue::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	TestFalse(TEXT("empty buffer rejects ConsumeFirst"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("empty buffer Size is 0"), Buffer.Size(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_003FifoOrder,
	"UEMMO.Tasks.M1_003.FifoOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_003FifoOrder::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	TestTrue(TEXT("push 1 accepted"), Buffer.Push(MakeInput(1, ECombatInput::Light)));
	TestTrue(TEXT("push 2 accepted"), Buffer.Push(MakeInput(2, ECombatInput::Light)));
	TestTrue(TEXT("push 3 accepted"), Buffer.Push(MakeInput(3, ECombatInput::Light)));
	TestEqual(TEXT("size is 3"), Buffer.Size(), 3);

	TestTrue(TEXT("first consume succeeds"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("first consumed sequence is 1"), Out.Sequence, uint64(1));
	TestTrue(TEXT("second consume succeeds"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("second consumed sequence is 2"), Out.Sequence, uint64(2));
	TestTrue(TEXT("third consume succeeds"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("third consumed sequence is 3"), Out.Sequence, uint64(3));
	TestEqual(TEXT("buffer empty after consuming all"), Buffer.Size(), 0);
	TestFalse(TEXT("consume from drained buffer fails"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_003ActionMatchKeepsOrder,
	"UEMMO.Tasks.M1_003.ActionMatchKeepsOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_003ActionMatchKeepsOrder::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	TestTrue(TEXT("push Light 1 accepted"), Buffer.Push(MakeInput(1, ECombatInput::Light)));
	TestTrue(TEXT("push Launcher 2 accepted"), Buffer.Push(MakeInput(2, ECombatInput::Launcher)));
	TestTrue(TEXT("push Light 3 accepted"), Buffer.Push(MakeInput(3, ECombatInput::Light)));

	TestFalse(TEXT("Jump not present"), Buffer.ConsumeFirst(ECombatInput::Jump, Out));
	TestEqual(TEXT("size unchanged after failed match"), Buffer.Size(), 3);

	TestTrue(TEXT("Launcher consumed"), Buffer.ConsumeFirst(ECombatInput::Launcher, Out));
	TestEqual(TEXT("consumed Launcher is sequence 2"), Out.Sequence, uint64(2));
	TestEqual(TEXT("remaining size is 2"), Buffer.Size(), 2);

	TestTrue(TEXT("Light consumed"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("first Light still sequence 1"), Out.Sequence, uint64(1));
	TestTrue(TEXT("next Light consumed"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("last Light is sequence 3"), Out.Sequence, uint64(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_003CapacityOverflow,
	"UEMMO.Tasks.M1_003.CapacityOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_003CapacityOverflow::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	for (uint64 Sequence = 1; Sequence <= 4; ++Sequence)
	{
		TestTrue(TEXT("push while below capacity"), Buffer.Push(MakeInput(Sequence, ECombatInput::Light)));
	}
	TestEqual(TEXT("size is 4 at capacity"), Buffer.Size(), 4);

	TestTrue(TEXT("push 5 accepted by dropping oldest"), Buffer.Push(MakeInput(5, ECombatInput::Light)));
	TestEqual(TEXT("size still 4 after overflow push"), Buffer.Size(), 4);

	TestTrue(TEXT("consume after overflow"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("earliest consumable sequence is 2"), Out.Sequence, uint64(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_003SingleConsumptionAndDuplicates,
	"UEMMO.Tasks.M1_003.SingleConsumptionAndDuplicates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_003SingleConsumptionAndDuplicates::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	TestTrue(TEXT("push Light 1 accepted"), Buffer.Push(MakeInput(1, ECombatInput::Light)));
	TestTrue(TEXT("single entry consumed once"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestEqual(TEXT("consumed sequence is 1"), Out.Sequence, uint64(1));
	TestFalse(TEXT("consuming the same entry again fails"), Buffer.ConsumeFirst(ECombatInput::Light, Out));

	TestTrue(TEXT("push Light 2 accepted"), Buffer.Push(MakeInput(2, ECombatInput::Light)));
	TestFalse(TEXT("duplicate sequence rejected"), Buffer.Push(MakeInput(2, ECombatInput::Light)));
	TestFalse(TEXT("regressed sequence rejected"), Buffer.Push(MakeInput(1, ECombatInput::Light)));
	TestEqual(TEXT("rejected pushes did not change size"), Buffer.Size(), 1);

	Buffer.Reset();
	TestEqual(TEXT("size is 0 after reset"), Buffer.Size(), 0);
	TestFalse(TEXT("consume after reset fails"), Buffer.ConsumeFirst(ECombatInput::Light, Out));
	TestTrue(TEXT("new session accepts sequence 1 again"), Buffer.Push(MakeInput(1, ECombatInput::Jump)));
	return true;
}

// ---------------------------------------------------------------------------
// M1-004: input expiry on an explicit monotonic input clock, plus reset cleanup.
// Boundary values verified in double precision:
//   1.150 - 1.0 = 0.1499999999999999 <= 0.150 (still valid)
//   1.151 - 1.0 = 0.15100000000000002 >  0.150 (expired)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_004ExpiryBoundary,
	"UEMMO.Tasks.M1_004.ExpiryBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_004ExpiryBoundary::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;

	// An input pressed at t=1.0 is still consumable at Now=1.150 (expiry needs strictly greater than 0.150).
	TestTrue(TEXT("push Light pressed at 1.0"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 1.0)));
	TestTrue(TEXT("consumable at Now=1.150"), Buffer.Consume(ECombatInput::Light, 1.150, TestLifetime, Out));
	TestEqual(TEXT("boundary consume returned sequence 1"), Out.Sequence, uint64(1));
	TestEqual(TEXT("buffer empty after boundary consume"), Buffer.Size(), 0);

	// The same input is expired at Now=1.151.
	TestTrue(TEXT("push Light pressed at 1.0 again"), Buffer.Push(MakeTimedInput(2, ECombatInput::Light, 1.0)));
	TestFalse(TEXT("not consumable at Now=1.151"), Buffer.Consume(ECombatInput::Light, 1.151, TestLifetime, Out));
	TestEqual(TEXT("expired entry removed by consume"), Buffer.Size(), 0);

	// PruneExpired alone removes the same expired entry.
	Buffer.Reset();
	TestTrue(TEXT("push Light pressed at 1.0 for prune"), Buffer.Push(MakeTimedInput(3, ECombatInput::Light, 1.0)));
	Buffer.PruneExpired(1.151, TestLifetime);
	TestEqual(TEXT("PruneExpired removed the expired entry"), Buffer.Size(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_004MixedActionPrune,
	"UEMMO.Tasks.M1_004.MixedActionPrune",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_004MixedActionPrune::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;

	// Light pressed long ago (expired at Now=1.0), Launcher pressed just now (still valid).
	TestTrue(TEXT("push stale Light"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 0.0)));
	TestTrue(TEXT("push fresh Launcher"), Buffer.Push(MakeTimedInput(2, ECombatInput::Launcher, 1.0)));

	Buffer.PruneExpired(1.0, TestLifetime);

	TestEqual(TEXT("only the expired entry was removed"), Buffer.Size(), 1);
	TestTrue(TEXT("valid Launcher still consumable after prune"), Buffer.Consume(ECombatInput::Launcher, 1.0, TestLifetime, Out));
	TestEqual(TEXT("surviving entry is sequence 2"), Out.Sequence, uint64(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_004NonFiniteAndFuture,
	"UEMMO.Tasks.M1_004.NonFiniteAndFuture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_004NonFiniteAndFuture::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	const double NaNTimestamp = std::numeric_limits<double>::quiet_NaN();
	const double InfiniteTimestamp = std::numeric_limits<double>::infinity();

	// Push rejects non-finite timestamps outright and must not advance the sequence watermark.
	TestFalse(TEXT("NaN PressedAt rejected by Push"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, NaNTimestamp)));
	TestEqual(TEXT("rejected NaN push did not change size"), Buffer.Size(), 0);
	TestTrue(TEXT("sequence watermark not advanced: sequence 1 still accepted"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 1.0)));

	// A clearly future timestamp is pruned by PruneExpired while the valid entry survives.
	TestTrue(TEXT("push future-dated entry"), Buffer.Push(MakeTimedInput(2, ECombatInput::Light, 2.0)));
	Buffer.PruneExpired(1.0, TestLifetime);
	TestEqual(TEXT("only the valid entry remains after pruning the future entry"), Buffer.Size(), 1);
	TestTrue(TEXT("valid entry still consumable"), Buffer.Consume(ECombatInput::Light, 1.0, TestLifetime, Out));
	TestEqual(TEXT("survivor is the valid sequence 1"), Out.Sequence, uint64(1));

	// Infinite timestamps are rejected as non-finite as well.
	TestFalse(TEXT("infinite PressedAt rejected by Push"), Buffer.Push(MakeTimedInput(2, ECombatInput::Light, InfiniteTimestamp)));
	TestEqual(TEXT("rejected infinite push did not change size"), Buffer.Size(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_004ConsumePrunesThenConsumes,
	"UEMMO.Tasks.M1_004.ConsumePrunesThenConsumes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_004ConsumePrunesThenConsumes::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;

	// Queue order: expired Light (1), valid Light (2), valid Launcher (3).
	TestTrue(TEXT("push expired Light"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 0.0)));
	TestTrue(TEXT("push valid Light"), Buffer.Push(MakeTimedInput(2, ECombatInput::Light, 1.0)));
	TestTrue(TEXT("push valid Launcher"), Buffer.Push(MakeTimedInput(3, ECombatInput::Launcher, 1.0)));

	// Consume must prune first: the expired Light is skipped and the valid Light (2) is consumed.
	TestTrue(TEXT("consume succeeds after pruning the expired entry"), Buffer.Consume(ECombatInput::Light, 1.0, TestLifetime, Out));
	TestEqual(TEXT("consumed the valid Light, not the expired one"), Out.Sequence, uint64(2));
	TestEqual(TEXT("expired Light removed, Launcher untouched"), Buffer.Size(), 1);
	TestTrue(TEXT("Launcher still consumable afterwards"), Buffer.Consume(ECombatInput::Launcher, 1.0, TestLifetime, Out));
	TestEqual(TEXT("Launcher consumed is sequence 3"), Out.Sequence, uint64(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_004DefaultLifetime,
	"UEMMO.Tasks.M1_004.DefaultLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_004DefaultLifetime::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;

	TestTrue(TEXT("push Light pressed at 1.0"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 1.0)));

	// PruneExpired without an explicit lifetime uses 0.150 seconds.
	Buffer.PruneExpired(1.10);
	TestEqual(TEXT("age 0.1 is within the default lifetime"), Buffer.Size(), 1);
	Buffer.PruneExpired(1.16);
	TestEqual(TEXT("age 0.16 exceeds the default lifetime"), Buffer.Size(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_004ResetStartsNewSession,
	"UEMMO.Tasks.M1_004.ResetStartsNewSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_004ResetStartsNewSession::RunTest(const FString& Parameters)
{
	FCombatInputBuffer Buffer;
	FBufferedCombatInput Out;
	const double NaNTimestamp = std::numeric_limits<double>::quiet_NaN();

	TestTrue(TEXT("push Light"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 1.0)));
	TestTrue(TEXT("push Launcher"), Buffer.Push(MakeTimedInput(2, ECombatInput::Launcher, 1.0)));
	TestEqual(TEXT("size before reset"), Buffer.Size(), 2);

	Buffer.Reset();
	TestEqual(TEXT("size is 0 after reset"), Buffer.Size(), 0);
	TestFalse(TEXT("consume fails after reset"), Buffer.Consume(ECombatInput::Light, 2.0, TestLifetime, Out));

	// The new session accepts sequence numbers from 1 again.
	TestTrue(TEXT("new session accepts sequence 1"), Buffer.Push(MakeTimedInput(1, ECombatInput::Light, 2.0)));
	TestTrue(TEXT("new session consumes the fresh input"), Buffer.Consume(ECombatInput::Light, 2.0, TestLifetime, Out));
	TestEqual(TEXT("new session consumed sequence 1"), Out.Sequence, uint64(1));

	// Non-finite timestamps stay rejected in the new session.
	TestFalse(TEXT("NaN still rejected after reset"), Buffer.Push(MakeTimedInput(2, ECombatInput::Launcher, NaNTimestamp)));
	TestEqual(TEXT("rejected NaN push did not change size"), Buffer.Size(), 0);
	return true;
}

#endif
