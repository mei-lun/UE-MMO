#include "Misc/AutomationTest.h"

#include "../Combat/CombatInputBuffer.h"

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

#endif
