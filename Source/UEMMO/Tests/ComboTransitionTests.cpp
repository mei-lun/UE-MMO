#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatInputBuffer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_014
{
	// The tests drive the component with single 1/60 s steps, so logic frame N
	// sits at N/60 s of game time. The input clock is injected explicitly
	// (interface contract section 2); TickCombat never advances it itself.
	constexpr double FrameSeconds = 1.0 / 60.0;

	// Builds a catalog from Config/DefaultGame.ini (the four explicit DA
	// references, the same loading path the M1-010/M1-011 tests use) and
	// injects it into a fresh combat component. Returns null after reporting
	// the failure so the caller can bail out early.
	static UCombatComponent* NewCombatComponentWithCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("InitializeFromConfig succeeds with the four explicit DefaultGame.ini references"),
			Catalog->InitializeFromConfig(Error)))
		{
			Test.AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
			return nullptr;
		}
		UCombatComponent* Component = NewObject<UCombatComponent>();
		Test.TestTrue(TEXT("InitializeFromCatalog accepts the initialized catalog"),
			Component->InitializeFromCatalog(Catalog));
		return Component;
	}

	// Counts Started/Finished broadcasts and keeps the last payloads so tests
	// can assert the switch semantics (Finished for the retired instance plus
	// a second Started for the chained one).
	struct FChainEvents
	{
		int32 StartedCount = 0;
		int32 FinishedCount = 0;
		FName StartedAttackId = NAME_None;
		uint64 StartedInstanceId = 0;
		FName FinishedAttackId = NAME_None;
		uint64 FinishedInstanceId = 0;

		void Bind(UCombatComponent& Component)
		{
			Component.OnStarted.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				++StartedCount;
				StartedAttackId = AttackId;
				StartedInstanceId = InstanceId;
			});
			Component.OnFinished.AddLambda([this](FName AttackId, uint64 InstanceId)
			{
				++FinishedCount;
				FinishedAttackId = AttackId;
				FinishedInstanceId = InstanceId;
			});
		}
	};

	// Advances the component by exactly Ticks single 1/60 s frames.
	static void TickN(UCombatComponent& Component, int32 Ticks)
	{
		for (int32 Index = 0; Index < Ticks; ++Index)
		{
			Component.TickCombat(static_cast<float>(FrameSeconds));
		}
	}

	// Queues one combat intent with an explicit press time (input game clock).
	static void QueueAction(UCombatComponent& Component, uint64 Sequence, ECombatInput Action, double PressedAt)
	{
		FBufferedCombatInput Input;
		Input.Sequence = Sequence;
		Input.Action = Action;
		Input.PressedAt = PressedAt;
		Component.QueueInput(Input);
	}
}

using namespace UE::UEMMO::Tasks::M1_014;

// A Light pressed at Frame 10 (before the [12,24) cancel window) chains on the
// first window step: the running light_01 finishes (one Finished broadcast),
// light_02 starts immediately with a fresh InstanceId and the same Facing, the
// consumed input leaves the buffer empty, and the chain tick does not advance
// the new attack (it steps from its own next TickCombat).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014EarlyPressChainsLight01IntoLight02AtWindowStart,
	"UEMMO.Tasks.M1_014.EarlyPressChainsLight01IntoLight02AtWindowStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014EarlyPressChainsLight01IntoLight02AtWindowStart::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Advance to Frame 10 and press J there (PressedAt = 10/60 s).
	TickN(*Component, 11);
	TestEqual(TEXT("precondition: the attack is at Frame 10"), Component->GetSnapshot().Frame, 10);
	Component->SetInputClockSeconds(10.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Light, 10.0 * FrameSeconds);
	TestEqual(TEXT("the early press is buffered"), Component->GetSnapshot().BufferSize, 1);

	// Frame 11 is still outside the cancel window [12,24): the buffer must be
	// untouched and the attack keeps running.
	TickN(*Component, 1);
	TestEqual(TEXT("Frame 11 is outside the cancel window and keeps the buffered Light"),
		Component->GetSnapshot().BufferSize, 1);
	TestEqual(TEXT("Frame 11 does not chain yet"), Component->GetSnapshot().InstanceId, uint64(1));

	// Frame 12 opens the cancel window; the input age is 2/60 s, well inside
	// the 150 ms lifetime, so the chain must fire on exactly this step.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the chain starts light_02"), Chained.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the chained instance mints a fresh InstanceId 2"), Chained.InstanceId, uint64(2));
	TestTrue(TEXT("the component is Attacking right after the chain"),
		Chained.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the chained instance keeps the running Facing"), Chained.Facing, 1);
	TestEqual(TEXT("the consumed input leaves the buffer empty"), Chained.BufferSize, 0);
	TestEqual(TEXT("the chain tick does not advance the new attack (Frame stays -1)"), Chained.Frame, -1);

	TestEqual(TEXT("the old instance broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("Finished carries the canceled light_01"), Events.FinishedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("Finished carries the old InstanceId 1"), Events.FinishedInstanceId, uint64(1));
	TestEqual(TEXT("the chain fires a second Started"), Events.StartedCount, 2);
	TestEqual(TEXT("the second Started carries light_02"), Events.StartedAttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the second Started carries InstanceId 2"), Events.StartedInstanceId, uint64(2));

	// The new instance steps from its own next tick; no frame is skipped and
	// no extra attack starts.
	TickN(*Component, 1);
	TestEqual(TEXT("the next tick lands light_02 on Frame 0"), Component->GetSnapshot().Frame, 0);
	TestEqual(TEXT("ticking on after the chain starts nothing new"), Events.StartedCount, 2);
	return true;
}

// A Light pressed at Frame 1 is older than the 150 ms lifetime when the cancel
// window opens: PruneExpired removes it before any consumption attempt, so the
// attack never chains and light_01 ends naturally back to Free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014ExpiredPressDoesNotChainAndLight01EndsNaturally,
	"UEMMO.Tasks.M1_014.ExpiredPressDoesNotChainAndLight01EndsNaturally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014ExpiredPressDoesNotChainAndLight01EndsNaturally::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Advance to Frame 1 and press J there.
	TickN(*Component, 2);
	TestEqual(TEXT("precondition: the attack is at Frame 1"), Component->GetSnapshot().Frame, 1);
	Component->SetInputClockSeconds(1.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Light, 1.0 * FrameSeconds);

	// At Frame 12 the input age is 11/60 s (about 183 ms), past the 150 ms
	// lifetime: the first window step prunes it without consuming anything.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 11);

	const FCombatSnapshot WindowStep = Component->GetSnapshot();
	TestEqual(TEXT("the expired press does not chain (still light_01)"), WindowStep.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("the expired press keeps the running InstanceId 1"), WindowStep.InstanceId, uint64(1));
	TestEqual(TEXT("the first window step lands on Frame 12"), WindowStep.Frame, 12);
	TestEqual(TEXT("the expired press was pruned, not consumed"), WindowStep.BufferSize, 0);
	TestEqual(TEXT("no extra Started fired"), Events.StartedCount, 1);
	TestEqual(TEXT("the attack is still running in its window"), Events.FinishedCount, 0);

	// Run out the remaining frames 13..25: light_01 ends naturally.
	TickN(*Component, 13);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("light_01 ends naturally back to Free"), Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("Finished carries the naturally ended light_01"), Events.FinishedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("Finished carries InstanceId 1"), Events.FinishedInstanceId, uint64(1));
	TestEqual(TEXT("no chain started a second attack"), Events.StartedCount, 1);
	TestEqual(TEXT("the buffer stays empty after the natural end"), Done.BufferSize, 0);

	// Ticking on stays Free and fires nothing.
	TickN(*Component, 3);
	TestTrue(TEXT("ticking after the natural end stays Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("ticking after the natural end fires no Finished"), Events.FinishedCount, 1);
	return true;
}

// An input whose age is exactly the 150 ms lifetime is still valid (only a
// strictly greater age expires): the press is kept by the window prune and the
// chain fires. The times are plain doubles chosen so the age is exact with no
// floating point drift (0.15 - 0.0 == 0.15).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014Exactly150msPressIsStillValid,
	"UEMMO.Tasks.M1_014.Exactly150msPressIsStillValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014Exactly150msPressIsStillValid::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	TickN(*Component, 11);
	TestEqual(TEXT("precondition: the attack is at Frame 10"), Component->GetSnapshot().Frame, 10);
	Component->SetInputClockSeconds(0.150);
	QueueAction(*Component, 1, ECombatInput::Light, 0.0);
	TestEqual(TEXT("the boundary press is buffered"), Component->GetSnapshot().BufferSize, 1);

	TickN(*Component, 1);
	TestEqual(TEXT("Frame 11 keeps the boundary press untouched"), Component->GetSnapshot().BufferSize, 1);

	// Frame 12: the age is exactly 0.150 s, which the lifetime rule keeps.
	Component->SetInputClockSeconds(0.150);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the exactly-150ms press still chains into light_02"), Chained.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the chained instance carries InstanceId 2"), Chained.InstanceId, uint64(2));
	TestEqual(TEXT("the consumed boundary press empties the buffer"), Chained.BufferSize, 0);
	TestEqual(TEXT("the boundary chain broadcasts Finished once"), Events.FinishedCount, 1);
	TestEqual(TEXT("the boundary chain fires a second Started"), Events.StartedCount, 2);
	return true;
}

// Two buffered Lights (two Sequences) chain exactly once and only the earliest
// Sequence is consumed: light_02 is Attacking afterwards with the second Light
// still buffered for its own (rejected) window check.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014DoublePressConsumesOnlyEarliestSequence,
	"UEMMO.Tasks.M1_014.DoublePressConsumesOnlyEarliestSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014DoublePressConsumesOnlyEarliestSequence::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Two rapid J presses around Frame 10/11 (Sequences 1 and 2).
	TickN(*Component, 11);
	Component->SetInputClockSeconds(11.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Light, 10.0 * FrameSeconds);
	QueueAction(*Component, 2, ECombatInput::Light, 11.0 * FrameSeconds);
	TestEqual(TEXT("both buffered presses sit in the queue"), Component->GetSnapshot().BufferSize, 2);

	TickN(*Component, 1);
	TestEqual(TEXT("Frame 11 still holds both presses"), Component->GetSnapshot().BufferSize, 2);

	// Frame 12: only the earliest Sequence (1) is consumed for the chain.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the double press chains exactly once into light_02"), Chained.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the chained instance carries InstanceId 2"), Chained.InstanceId, uint64(2));
	TestTrue(TEXT("light_02 is Attacking right after the chain"),
		Chained.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("exactly one press was consumed, one remains"), Chained.BufferSize, 1);
	TestEqual(TEXT("only one Finished fired for the single switch"), Events.FinishedCount, 1);
	TestEqual(TEXT("only a second Started fired for the single switch"), Events.StartedCount, 2);

	FBufferedCombatInput Remaining;
	TestTrue(TEXT("the remaining press is readable"), Component->PeekInputBuffer(Remaining, 0));
	TestEqual(TEXT("the remaining press is the later Sequence 2"), Remaining.Sequence, uint64(2));
	TestTrue(TEXT("the remaining press is still a Light"), Remaining.Action == ECombatInput::Light);
	TestTrue(TEXT("the remaining press keeps its own press time"),
		FMath::IsNearlyEqual(Remaining.PressedAt, 11.0 * FrameSeconds));

	// The leftover Light does not skip light_02 forward: the next tick steps
	// the new instance from Frame 0 and starts nothing.
	TickN(*Component, 1);
	TestEqual(TEXT("the next tick lands light_02 on Frame 0"), Component->GetSnapshot().Frame, 0);
	TestEqual(TEXT("the leftover press starts no extra attack"), Events.StartedCount, 2);
	return true;
}

// A press at Frame 5, well before the [12,24) window, is not touched while
// Frame < 12 and is consumed on the first window step (its age there is
// 7/60 s, inside the lifetime).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014PressBeforeWindowStaysBufferedUntilWindowOpens,
	"UEMMO.Tasks.M1_014.PressBeforeWindowStaysBufferedUntilWindowOpens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014PressBeforeWindowStaysBufferedUntilWindowOpens::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Press at Frame 5 with the input clock following the frame grid.
	TickN(*Component, 6);
	TestEqual(TEXT("precondition: the attack is at Frame 5"), Component->GetSnapshot().Frame, 5);
	Component->SetInputClockSeconds(5.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Light, 5.0 * FrameSeconds);

	// Frames 6..8 and 9..11: outside the window, the buffer stays untouched
	// even though the input clock keeps running.
	Component->SetInputClockSeconds(8.0 * FrameSeconds);
	TickN(*Component, 3);
	TestEqual(TEXT("Frame 8 is before the window and keeps the press"), Component->GetSnapshot().BufferSize, 1);
	TestEqual(TEXT("Frame 8 does not chain"), Component->GetSnapshot().InstanceId, uint64(1));
	Component->SetInputClockSeconds(11.0 * FrameSeconds);
	TickN(*Component, 3);
	TestEqual(TEXT("Frame 11 is still before the window and keeps the press"),
		Component->GetSnapshot().BufferSize, 1);
	TestEqual(TEXT("Frame 11 still runs the original instance"), Component->GetSnapshot().InstanceId, uint64(1));
	TestEqual(TEXT("no event fired before the window"), Events.StartedCount + Events.FinishedCount, 1);

	// Frame 12 opens the window: the press (age 7/60 s) chains immediately.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the window-opening step chains into light_02"), Chained.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the chained instance carries InstanceId 2"), Chained.InstanceId, uint64(2));
	TestEqual(TEXT("the consumed press empties the buffer"), Chained.BufferSize, 0);
	TestEqual(TEXT("the chain broadcast Finished once for light_01"), Events.FinishedCount, 1);
	TestEqual(TEXT("the chain fired a second Started"), Events.StartedCount, 2);
	return true;
}

// A Launcher press inside the light_01 window is never consumed by the Light
// chaining: the buffer keeps it through every window step and the natural end
// (its press time stays inside the lifetime, isolating the action mismatch
// from the expiry rule), and nothing chains.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014LauncherInputIsNotConsumedDuringLight01Window,
	"UEMMO.Tasks.M1_014.LauncherInputIsNotConsumedDuringLight01Window",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014LauncherInputIsNotConsumedDuringLight01Window::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Press K at Frame 11; the input clock then stays at 14/60 s for the rest
	// of the attack, so the Launcher never ages past its 150 ms lifetime.
	TickN(*Component, 12);
	TestEqual(TEXT("precondition: the attack is at Frame 11"), Component->GetSnapshot().Frame, 11);
	Component->SetInputClockSeconds(11.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 11.0 * FrameSeconds);
	Component->SetInputClockSeconds(14.0 * FrameSeconds);

	// First window step: a Launcher is not a Light, so nothing is consumed
	// and nothing chains.
	TickN(*Component, 1);
	const FCombatSnapshot WindowStep = Component->GetSnapshot();
	TestEqual(TEXT("the Launcher press does not chain (still light_01)"), WindowStep.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("the Launcher press keeps the running InstanceId 1"), WindowStep.InstanceId, uint64(1));
	TestEqual(TEXT("the Launcher press survives the first window step"), WindowStep.BufferSize, 1);
	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the buffered press is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestTrue(TEXT("the buffered press is the Launcher"), Buffered.Action == ECombatInput::Launcher);
	TestEqual(TEXT("the buffered Launcher keeps Sequence 1"), Buffered.Sequence, uint64(1));

	// Run through the rest of the window and the natural end: no step consumes
	// the Launcher and nothing chains.
	TickN(*Component, 13);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("light_01 ends naturally with only a Launcher buffered"),
		Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("no chain fired a second Started"), Events.StartedCount, 1);
	TestEqual(TEXT("the Launcher is still buffered after the natural end"), Done.BufferSize, 1);
	TestTrue(TEXT("the surviving entry is still the Launcher"),
		Component->PeekInputBuffer(Buffered, 0) && Buffered.Action == ECombatInput::Launcher);
	return true;
}

// A Light pressed inside light_02's own [16,29) window cannot chain (its
// AllowedNextAttacks hold no light_02 follow-up): the input is kept, not
// consumed, and light_02 runs out naturally with the press still buffered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_014LightInLight02WindowIsKeptAndAttackEndsNaturally,
	"UEMMO.Tasks.M1_014.LightInLight02WindowIsKeptAndAttackEndsNaturally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_014LightInLight02WindowIsKeptAndAttackEndsNaturally::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FChainEvents Events;
	Events.Bind(*Component);

	// Chain into light_02 first (press at Frame 10 of light_01, chain at 12).
	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TickN(*Component, 11);
	Component->SetInputClockSeconds(10.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Light, 10.0 * FrameSeconds);
	TickN(*Component, 1);
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);
	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("precondition: the chain started light_02"), Chained.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("precondition: light_02 runs as InstanceId 2"), Chained.InstanceId, uint64(2));

	// Advance to Frame 16 where light_02's cancel window [16,29) opens and
	// press J there.
	TickN(*Component, 17);
	TestEqual(TEXT("precondition: light_02 is at Frame 16"), Component->GetSnapshot().Frame, 16);
	Component->SetInputClockSeconds(28.0 * FrameSeconds);
	QueueAction(*Component, 2, ECombatInput::Light, 28.0 * FrameSeconds);

	// The input clock stays at 29/60 s from here (age 1/60 s), so the press
	// never ages out and the keep-behavior is isolated from the lifetime rule.
	Component->SetInputClockSeconds(29.0 * FrameSeconds);

	// First light_02 window step: the Light finds no allowed light follow-up
	// and must stay buffered.
	TickN(*Component, 1);
	const FCombatSnapshot Kept = Component->GetSnapshot();
	TestEqual(TEXT("the rejected Light does not chain (still light_02)"), Kept.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the rejected Light keeps the running InstanceId 2"), Kept.InstanceId, uint64(2));
	TestEqual(TEXT("the rejected Light is still buffered"), Kept.BufferSize, 1);
	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the rejected press is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestEqual(TEXT("the rejected press keeps Sequence 2"), Buffered.Sequence, uint64(2));
	TestTrue(TEXT("the rejected press is still a Light"), Buffered.Action == ECombatInput::Light);
	TestEqual(TEXT("the rejected press fired no Finished yet"), Events.FinishedCount, 1);

	// Run out the remaining frames: light_02 ends naturally with the press
	// still buffered (kept for later consumers).
	TickN(*Component, 14);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("light_02 ends naturally back to Free"), Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end is the second Finished"), Events.FinishedCount, 2);
	TestEqual(TEXT("the natural end carries light_02"), Events.FinishedAttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the natural end carries InstanceId 2"), Events.FinishedInstanceId, uint64(2));
	TestEqual(TEXT("no chain ever fired a third Started"), Events.StartedCount, 2);
	TestEqual(TEXT("the rejected Light survives the natural end"), Done.BufferSize, 1);
	TestTrue(TEXT("the surviving entry is still a Light"),
		Component->PeekInputBuffer(Buffered, 0) && Buffered.Action == ECombatInput::Light);
	return true;
}

#endif
