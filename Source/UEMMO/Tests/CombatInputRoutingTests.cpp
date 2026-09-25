#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "EnhancedInputComponent.h"

#include <limits>

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatInputBuffer.h"
#include "../PrototypeCharacter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_012
{
	// NewObject keeps the tests hermetic: no level load, no BeginPlay, no world.
	// Default subobjects (capsule, movement, combat component) still exist, so
	// the J/K routing is exercised exactly as shipped. Times are passed
	// explicitly (interface contract section 2: early tests use explicit times;
	// the world-less game-clock overload reads 0.0 and is covered separately).
	static APrototypeCharacter* MakeCharacter()
	{
		return NewObject<APrototypeCharacter>();
	}

	// Collects the action pointers of every Started-trigger binding on the
	// component, in registration order. Used to prove that a repeated input
	// setup neither stacks duplicates nor rebuilds the runtime actions.
	static TArray<const UInputAction*> CollectStartedActions(UEnhancedInputComponent& Input)
	{
		TArray<const UInputAction*> Actions;
		for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input.GetActionEventBindings())
		{
			if (Binding.IsValid() && Binding->GetTriggerEvent() == ETriggerEvent::Started)
			{
				Actions.Add(Binding->GetAction());
			}
		}
		return Actions;
	}
}

using namespace UE::UEMMO::Tasks::M1_012;

// One SubmitCombatInput(Light) buffers exactly one intent with Sequence 1,
// the Light action and the explicitly passed PressedAt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_012SubmitLightOnceBuffersSingleIntent,
	"UEMMO.Tasks.M1_012.SubmitLightOnceBuffersSingleIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_012SubmitLightOnceBuffersSingleIntent::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UCombatComponent* Combat = Character->GetCombat();
	TestNotNull(TEXT("the character owns a combat component subobject"), Combat);
	if (Combat == nullptr)
	{
		return true;
	}

	Character->SubmitCombatInput(ECombatInput::Light, 1.25);

	const FCombatSnapshot Snapshot = Combat->GetSnapshot();
	TestEqual(TEXT("one submit buffers exactly one intent"), Snapshot.BufferSize, 1);
	FBufferedCombatInput Entry;
	TestTrue(TEXT("PeekInputBuffer(0) reads the buffered intent"), Combat->PeekInputBuffer(Entry, 0));
	TestEqual(TEXT("the first intent carries Sequence 1"), Entry.Sequence, uint64(1));
	TestTrue(TEXT("the J press buffers the Light action"), Entry.Action == ECombatInput::Light);
	TestTrue(TEXT("PressedAt is positive"), Entry.PressedAt > 0.0);
	TestTrue(TEXT("PressedAt is exactly the explicitly passed input game time"),
		FMath::IsNearlyEqual(Entry.PressedAt, 1.25));
	return true;
}

// Consecutive J/K/J presses buffer three intents whose sequences strictly
// increase 1/2/3 and whose actions alternate Light/Launcher/Light.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_012ConsecutivePressesIncrementSequenceAndAlternateAction,
	"UEMMO.Tasks.M1_012.ConsecutivePressesIncrementSequenceAndAlternateAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_012ConsecutivePressesIncrementSequenceAndAlternateAction::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UCombatComponent* Combat = Character->GetCombat();
	if (Combat == nullptr)
	{
		TestNotNull(TEXT("precondition: the character owns a combat component"), Combat);
		return true;
	}

	Character->SubmitCombatInput(ECombatInput::Light, 10.0);	// J press
	Character->SubmitCombatInput(ECombatInput::Launcher, 10.1);	// K press
	Character->SubmitCombatInput(ECombatInput::Light, 10.2);	// J press again

	TestEqual(TEXT("three submits buffer exactly three intents"), Combat->GetSnapshot().BufferSize, 3);
	const ECombatInput ExpectedActions[] = { ECombatInput::Light, ECombatInput::Launcher, ECombatInput::Light };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FBufferedCombatInput Entry;
		TestTrue(FString::Printf(TEXT("entry %d is readable without consuming it"), Index),
			Combat->PeekInputBuffer(Entry, Index));
		TestEqual(FString::Printf(TEXT("entry %d carries Sequence %d"), Index, Index + 1),
			Entry.Sequence, uint64(Index + 1));
		TestTrue(FString::Printf(TEXT("entry %d carries the J/K alternation"), Index),
			Entry.Action == ExpectedActions[Index]);
		TestTrue(FString::Printf(TEXT("entry %d keeps its own press time"), Index),
			FMath::IsNearlyEqual(Entry.PressedAt, 10.0 + 0.1 * Index));
	}
	return true;
}

// The buffer behind QueueInput rejects a repeated sequence, a regressing
// sequence and a non-finite timestamp; only fresh, finite, increasing
// sequences grow the buffer (asserted through the component's public API).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_012QueueInputRejectsDuplicateAndRegressingSequences,
	"UEMMO.Tasks.M1_012.QueueInputRejectsDuplicateAndRegressingSequences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_012QueueInputRejectsDuplicateAndRegressingSequences::RunTest(const FString& Parameters)
{
	UCombatComponent* Combat = NewObject<UCombatComponent>();

	FBufferedCombatInput First;
	First.Sequence = 1;
	First.Action = ECombatInput::Light;
	First.PressedAt = 5.0;
	Combat->QueueInput(First);
	TestEqual(TEXT("the first queue input is buffered"), Combat->GetSnapshot().BufferSize, 1);

	Combat->QueueInput(First);	// same sequence again: rejected by the session rule
	TestEqual(TEXT("re-queueing the same sequence does not grow the buffer"),
		Combat->GetSnapshot().BufferSize, 1);

	FBufferedCombatInput Regressing;
	Regressing.Sequence = 0;	// below the accepted watermark: rejected
	Regressing.Action = ECombatInput::Launcher;
	Regressing.PressedAt = 5.1;
	Combat->QueueInput(Regressing);
	TestEqual(TEXT("a regressing sequence does not grow the buffer"),
		Combat->GetSnapshot().BufferSize, 1);

	FBufferedCombatInput NonFinite;
	NonFinite.Sequence = 9;
	NonFinite.Action = ECombatInput::Light;
	NonFinite.PressedAt = std::numeric_limits<double>::quiet_NaN();
	Combat->QueueInput(NonFinite);
	TestEqual(TEXT("a non-finite press time does not grow the buffer"),
		Combat->GetSnapshot().BufferSize, 1);

	FBufferedCombatInput Next;
	Next.Sequence = 2;
	Next.Action = ECombatInput::Launcher;
	Next.PressedAt = 5.2;
	Combat->QueueInput(Next);
	TestEqual(TEXT("a fresh increasing sequence grows the buffer"),
		Combat->GetSnapshot().BufferSize, 2);
	FBufferedCombatInput Second;
	TestTrue(TEXT("the second entry is readable at index 1"), Combat->PeekInputBuffer(Second, 1));
	TestEqual(TEXT("the second entry carries Sequence 2"), Second.Sequence, uint64(2));
	TestTrue(TEXT("the second entry carries the Launcher action"), Second.Action == ECombatInput::Launcher);
	return true;
}

// Running the input setup twice against the same input component (the
// re-initialization / re-possess hazard) neither stacks duplicate bindings nor
// rebuilds the runtime actions, and one submit still buffers exactly one
// intent with Sequence 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_012RepeatedSetupDoesNotDuplicateBindingsOrIntents,
	"UEMMO.Tasks.M1_012.RepeatedSetupDoesNotDuplicateBindingsOrIntents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_012RepeatedSetupDoesNotDuplicateBindingsOrIntents::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();

	Character->SetupPlayerInputComponent(Input);
	const TArray<const UInputAction*> StartedBefore = CollectStartedActions(*Input);
	TestTrue(TEXT("the first setup binds the Started events (jump, reset, light, launcher)"),
		StartedBefore.Num() >= 4);

	Character->SetupPlayerInputComponent(Input);
	const TArray<const UInputAction*> StartedAfter = CollectStartedActions(*Input);
	TestEqual(TEXT("a second setup does not add a single Started binding"),
		StartedAfter.Num(), StartedBefore.Num());
	bool bSameActionObjects = StartedAfter.Num() == StartedBefore.Num();
	for (int32 Index = 0; bSameActionObjects && Index < StartedAfter.Num(); ++Index)
	{
		bSameActionObjects = (StartedAfter[Index] == StartedBefore[Index]);
	}
	TestTrue(TEXT("a second setup reuses the same runtime action objects (nothing rebuilt)"),
		bSameActionObjects);

	Character->SubmitCombatInput(ECombatInput::Light, 2.0);
	UCombatComponent* Combat = Character->GetCombat();
	TestNotNull(TEXT("precondition: the character owns a combat component"), Combat);
	if (Combat)
	{
		TestEqual(TEXT("after the double setup one submit still buffers exactly one intent"),
			Combat->GetSnapshot().BufferSize, 1);
		FBufferedCombatInput Entry;
		TestTrue(TEXT("the intent is readable"), Combat->PeekInputBuffer(Entry, 0));
		TestEqual(TEXT("the double setup left the sequence counter untouched"), Entry.Sequence, uint64(1));
	}
	return true;
}

// The character tick drives TickCombat; ticking a Free component must never
// consume or reorder buffered intents (wiring smoke: no crash, sizes agree).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_012CombatTickKeepsBufferedIntentsConsistent,
	"UEMMO.Tasks.M1_012.CombatTickKeepsBufferedIntentsConsistent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_012CombatTickKeepsBufferedIntentsConsistent::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UCombatComponent* Combat = Character->GetCombat();
	if (Combat == nullptr)
	{
		TestNotNull(TEXT("precondition: the character owns a combat component"), Combat);
		return true;
	}

	Character->SubmitCombatInput(ECombatInput::Light, 1.0);
	Character->SubmitCombatInput(ECombatInput::Launcher, 1.05);
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Combat->TickCombat(1.0f / 60.0f);
	}
	TestEqual(TEXT("ticking a Free component consumes nothing"), Combat->GetSnapshot().BufferSize, 2);
	FBufferedCombatInput First;
	TestTrue(TEXT("the earliest intent survives the ticks unchanged"), Combat->PeekInputBuffer(First, 0));
	TestEqual(TEXT("the earliest intent keeps Sequence 1"), First.Sequence, uint64(1));
	return true;
}

// The game-clock overload (no explicit time) still routes exactly one intent;
// world-less it reads 0.0, which the buffer accepts as a finite timestamp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_012GameClockOverloadRoutesSingleIntent,
	"UEMMO.Tasks.M1_012.GameClockOverloadRoutesSingleIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_012GameClockOverloadRoutesSingleIntent::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UCombatComponent* Combat = Character->GetCombat();
	if (Combat == nullptr)
	{
		TestNotNull(TEXT("precondition: the character owns a combat component"), Combat);
		return true;
	}

	Character->SubmitCombatInput(ECombatInput::Light);
	TestEqual(TEXT("the game-clock overload buffers exactly one intent"),
		Combat->GetSnapshot().BufferSize, 1);
	return true;
}

#endif
