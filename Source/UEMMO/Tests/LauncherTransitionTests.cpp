#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatInputBuffer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_021
{
	// The tests drive the component with single 1/60 s steps, so logic frame N
	// sits at N/60 s of game time. The input clock is injected explicitly
	// (interface contract section 2); TickCombat never advances it itself.
	constexpr double FrameSeconds = 1.0 / 60.0;

	// Builds a catalog from Config/DefaultGame.ini (the four explicit DA
	// references, the same loading path the M1-010/M1-011/M1-014 tests use) and
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
	// can assert the switch semantics (exactly one Finished for the retired
	// instance plus one Started per started instance).
	struct FTransitionEvents
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

using namespace UE::UEMMO::Tasks::M1_021;

// A valid Launcher press buffered while Free starts the launcher attack on the
// next tick: fresh InstanceId, empty buffer, exactly one Started, no Finished.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021FreeLauncherPressStartsLauncher,
	"UEMMO.Tasks.M1_021.FreeLauncherPressStartsLauncher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021FreeLauncherPressStartsLauncher::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	Component->SetInputClockSeconds(1.0);
	QueueAction(*Component, 1, ECombatInput::Launcher, 1.0);
	TestEqual(TEXT("the K press is buffered while Free"), Component->GetSnapshot().BufferSize, 1);

	// One Free tick consumes the press and starts its mapped attack.
	Component->TickCombat(static_cast<float>(FrameSeconds));

	const FCombatSnapshot Started = Component->GetSnapshot();
	TestEqual(TEXT("the Free K press starts launcher"), Started.AttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("the input-driven start mints a fresh InstanceId 1"), Started.InstanceId, uint64(1));
	TestTrue(TEXT("the component is Attacking right after the input-driven start"),
		Started.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the started instance has not stepped yet (Frame stays -1)"), Started.Frame, -1);
	TestEqual(TEXT("the consumed press empties the buffer"), Started.BufferSize, 0);
	TestEqual(TEXT("the input-driven start carries the component's stored Facing"),
		Started.Facing, 0);

	TestEqual(TEXT("exactly one Started broadcast fired"), Events.StartedCount, 1);
	TestEqual(TEXT("Started carries launcher"), Events.StartedAttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("Started carries InstanceId 1"), Events.StartedInstanceId, uint64(1));
	TestEqual(TEXT("no Finished broadcast fired at the start"), Events.FinishedCount, 0);
	return true;
}

// A valid Light press buffered while Free starts light_01 on the next tick
// (the Free Light mapping), mirroring the Free Launcher behavior.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021FreeLightPressStartsLight01,
	"UEMMO.Tasks.M1_021.FreeLightPressStartsLight01",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021FreeLightPressStartsLight01::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	Component->SetInputClockSeconds(0.5);
	QueueAction(*Component, 1, ECombatInput::Light, 0.5);
	Component->TickCombat(static_cast<float>(FrameSeconds));

	const FCombatSnapshot Started = Component->GetSnapshot();
	TestEqual(TEXT("the Free J press starts light_01"), Started.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("the input-driven start mints InstanceId 1"), Started.InstanceId, uint64(1));
	TestTrue(TEXT("the component is Attacking after the start"),
		Started.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the consumed press empties the buffer"), Started.BufferSize, 0);
	TestEqual(TEXT("exactly one Started fired"), Events.StartedCount, 1);
	TestEqual(TEXT("Started carries light_01"), Events.StartedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("no Finished fired"), Events.FinishedCount, 0);
	return true;
}

// A Jump press while Free starts nothing on this card's scope: the entry stays
// buffered untouched for its later consumer (M1-023 owns the jump cancel).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021FreeJumpPressIsIgnoredAndKept,
	"UEMMO.Tasks.M1_021.FreeJumpPressIsIgnoredAndKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021FreeJumpPressIsIgnoredAndKept::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	Component->SetInputClockSeconds(1.0);
	QueueAction(*Component, 1, ECombatInput::Jump, 1.0);
	Component->TickCombat(static_cast<float>(FrameSeconds));

	const FCombatSnapshot After = Component->GetSnapshot();
	TestTrue(TEXT("the Jump press starts nothing (stays Free)"),
		After.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the Jump press stays buffered"), After.BufferSize, 1);
	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the buffered press is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestTrue(TEXT("the buffered press is the Jump"), Buffered.Action == ECombatInput::Jump);
	TestEqual(TEXT("no Started fired for a Jump press"), Events.StartedCount, 0);
	return true;
}

// A Launcher pressed at Frame 11 of light_01 chains on the first cancel-window
// step (Frame 12): the running light_01 finishes exactly once, launcher starts
// immediately with a fresh InstanceId and the same Facing, and the consumed
// press leaves the buffer empty. No animation Notify is involved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021Light01CancelWindowLauncherChainsLauncher,
	"UEMMO.Tasks.M1_021.Light01CancelWindowLauncherChainsLauncher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021Light01CancelWindowLauncherChainsLauncher::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Press K at Frame 11, before the [12,24) cancel window opens.
	TickN(*Component, 12);
	TestEqual(TEXT("precondition: the attack is at Frame 11"), Component->GetSnapshot().Frame, 11);
	Component->SetInputClockSeconds(11.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 11.0 * FrameSeconds);
	TestEqual(TEXT("the K press is buffered"), Component->GetSnapshot().BufferSize, 1);

	// Frame 12 opens the cancel window; the press age is 1/60 s, well inside
	// the 150 ms lifetime, so the switch fires on exactly this step.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the cancel window switch starts launcher"), Chained.AttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("the chained instance mints a fresh InstanceId 2"), Chained.InstanceId, uint64(2));
	TestTrue(TEXT("the component is Attacking right after the switch"),
		Chained.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the chained instance keeps the running Facing"), Chained.Facing, 1);
	TestEqual(TEXT("the consumed press empties the buffer"), Chained.BufferSize, 0);
	TestEqual(TEXT("the switch tick does not advance the new attack (Frame stays -1)"), Chained.Frame, -1);

	TestEqual(TEXT("the old instance broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("Finished carries the retired light_01"), Events.FinishedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("Finished carries the old InstanceId 1"), Events.FinishedInstanceId, uint64(1));
	TestEqual(TEXT("the switch fires a second Started"), Events.StartedCount, 2);
	TestEqual(TEXT("the second Started carries launcher"), Events.StartedAttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("the second Started carries InstanceId 2"), Events.StartedInstanceId, uint64(2));

	// The new instance steps from its own next tick; nothing else starts.
	TickN(*Component, 1);
	TestEqual(TEXT("the next tick lands launcher on Frame 0"), Component->GetSnapshot().Frame, 0);
	TestEqual(TEXT("ticking on after the switch starts nothing new"), Events.StartedCount, 2);
	return true;
}

// Card acceptance: a valid K on Frame 18 of light_02 (inside its [16,29)
// cancel window) switches into launcher with the M1-014 switch semantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021Light02Frame18LauncherChainsLauncher,
	"UEMMO.Tasks.M1_021.Light02Frame18LauncherChainsLauncher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021Light02Frame18LauncherChainsLauncher::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_02 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_02")), -1));

	// Advance to Frame 17 and press K there.
	TickN(*Component, 18);
	TestEqual(TEXT("precondition: light_02 is at Frame 17"), Component->GetSnapshot().Frame, 17);
	Component->SetInputClockSeconds(17.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 17.0 * FrameSeconds);

	// Frame 18 sits inside light_02's cancel window [16,29); the press age is
	// 1/60 s, so the switch fires on this step.
	Component->SetInputClockSeconds(18.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the Frame 18 K switches light_02 into launcher"), Chained.AttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("the chained instance mints a fresh InstanceId 2"), Chained.InstanceId, uint64(2));
	TestTrue(TEXT("the component is Attacking right after the switch"),
		Chained.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the chained instance keeps the running Facing"), Chained.Facing, -1);
	TestEqual(TEXT("the consumed press empties the buffer"), Chained.BufferSize, 0);
	TestEqual(TEXT("the switch tick does not advance the new attack"), Chained.Frame, -1);

	TestEqual(TEXT("the old instance broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("Finished carries the retired light_02"), Events.FinishedAttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("Finished carries the old InstanceId 1"), Events.FinishedInstanceId, uint64(1));
	TestEqual(TEXT("the switch fires a second Started"), Events.StartedCount, 2);
	TestEqual(TEXT("the second Started carries launcher"), Events.StartedAttackId, FName(TEXT("launcher")));
	return true;
}

// A K pressed at Frame 3 of light_02 is older than the 150 ms lifetime when
// the cancel window opens (Frame 16): PruneExpired removes it before any
// consumption attempt, nothing switches, and light_02 ends naturally with no
// launcher started afterwards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021ExpiredLauncherPressDoesNotChainAndLight02EndsNaturally,
	"UEMMO.Tasks.M1_021.ExpiredLauncherPressDoesNotChainAndLight02EndsNaturally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021ExpiredLauncherPressDoesNotChainAndLight02EndsNaturally::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_02 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_02")), 1));

	// Press K at Frame 3.
	TickN(*Component, 4);
	TestEqual(TEXT("precondition: light_02 is at Frame 3"), Component->GetSnapshot().Frame, 3);
	Component->SetInputClockSeconds(3.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 3.0 * FrameSeconds);
	TestEqual(TEXT("the early K press is buffered"), Component->GetSnapshot().BufferSize, 1);

	// At Frame 16 the press age is 13/60 s (about 217 ms), past the 150 ms
	// lifetime: the first window step prunes it without consuming anything.
	Component->SetInputClockSeconds(16.0 * FrameSeconds);
	TickN(*Component, 13);

	const FCombatSnapshot WindowStep = Component->GetSnapshot();
	TestEqual(TEXT("the expired press does not switch (still light_02)"), WindowStep.AttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("the expired press keeps the running InstanceId 1"), WindowStep.InstanceId, uint64(1));
	TestEqual(TEXT("the first window step lands on Frame 16"), WindowStep.Frame, 16);
	TestEqual(TEXT("the expired press was pruned, not consumed"), WindowStep.BufferSize, 0);
	TestEqual(TEXT("no extra Started fired"), Events.StartedCount, 1);
	TestEqual(TEXT("the attack is still running in its window"), Events.FinishedCount, 0);

	// Run out the remaining frames 17..31: light_02 ends naturally.
	TickN(*Component, 15);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("light_02 ends naturally back to Free"), Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("Finished carries the naturally ended light_02"), Events.FinishedAttackId, FName(TEXT("light_02")));
	TestEqual(TEXT("no chain started a second attack"), Events.StartedCount, 1);
	TestEqual(TEXT("the buffer stays empty after the natural end"), Done.BufferSize, 0);

	// Ticking on while Free starts nothing (the expired press is long gone).
	TickN(*Component, 2);
	TestTrue(TEXT("ticking after the natural end stays Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("ticking after the natural end fires no Started"), Events.StartedCount, 1);
	return true;
}

// With both a Launcher and a Light valid inside light_01's cancel window,
// exactly one entry is consumed: the earliest Sequence wins (first come first
// served) and there is no double switch. The later entry stays buffered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021SameFrameLightAndLauncherConsumesEarliestSequenceOnly,
	"UEMMO.Tasks.M1_021.SameFrameLightAndLauncherConsumesEarliestSequenceOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021SameFrameLightAndLauncherConsumesEarliestSequenceOnly::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Two same-frame presses around Frame 11: the Launcher first (Sequence 1),
	// then the Light (Sequence 2). Both map to follow-ups light_01 allows.
	TickN(*Component, 12);
	Component->SetInputClockSeconds(11.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 11.0 * FrameSeconds);
	QueueAction(*Component, 2, ECombatInput::Light, 11.0 * FrameSeconds);
	TestEqual(TEXT("both buffered presses sit in the queue"), Component->GetSnapshot().BufferSize, 2);

	// Frame 12: the earliest Sequence (1, the Launcher) is consumed; the Light
	// stays buffered for its own later evaluation.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Chained = Component->GetSnapshot();
	TestEqual(TEXT("the earliest Sequence (Launcher) wins the switch"), Chained.AttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("the chained instance carries InstanceId 2"), Chained.InstanceId, uint64(2));
	TestTrue(TEXT("the component is Attacking right after the single switch"),
		Chained.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("exactly one press was consumed, one remains"), Chained.BufferSize, 1);
	TestEqual(TEXT("only one Finished fired for the single switch"), Events.FinishedCount, 1);
	TestEqual(TEXT("only a second Started fired for the single switch"), Events.StartedCount, 2);

	FBufferedCombatInput Remaining;
	TestTrue(TEXT("the remaining press is readable"), Component->PeekInputBuffer(Remaining, 0));
	TestEqual(TEXT("the remaining press is the later Sequence 2"), Remaining.Sequence, uint64(2));
	TestTrue(TEXT("the remaining press is the Light"), Remaining.Action == ECombatInput::Light);

	// The leftover Light does not skip the new instance forward and starts
	// nothing by itself (launcher's window is far away and a Light maps to
	// light_02, which launcher does not allow).
	TickN(*Component, 1);
	TestEqual(TEXT("the next tick lands launcher on Frame 0"), Component->GetSnapshot().Frame, 0);
	TestEqual(TEXT("the leftover press starts no extra attack"), Events.StartedCount, 2);
	return true;
}

// One consumed K press can never start launcher twice: the consumption removes
// the entry from the buffer, so the Free ticks after the start (and after the
// natural end) find nothing to consume and no second launcher starts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021SameLauncherPressNeverStartsTwice,
	"UEMMO.Tasks.M1_021.SameLauncherPressNeverStartsTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021SameLauncherPressNeverStartsTwice::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	Component->SetInputClockSeconds(2.0);
	QueueAction(*Component, 1, ECombatInput::Launcher, 2.0);
	Component->TickCombat(static_cast<float>(FrameSeconds));
	TestEqual(TEXT("precondition: the Free K press started launcher"), Component->GetSnapshot().AttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("precondition: the consumed press emptied the buffer"), Component->GetSnapshot().BufferSize, 0);
	TestEqual(TEXT("precondition: exactly one Started fired"), Events.StartedCount, 1);

	// Ticks while the launcher runs cannot re-consume anything (buffer empty).
	TickN(*Component, 3);
	TestEqual(TEXT("the running launcher keeps InstanceId 1"), Component->GetSnapshot().InstanceId, uint64(1));
	TestEqual(TEXT("the buffer stays empty while attacking"), Component->GetSnapshot().BufferSize, 0);
	TestEqual(TEXT("no second Started while attacking"), Events.StartedCount, 1);

	// Run out the remaining frames 3..39: launcher ends naturally (duration 40).
	TickN(*Component, 37);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("launcher ends naturally back to Free"), Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("the natural end carries launcher"), Events.FinishedAttackId, FName(TEXT("launcher")));

	// Free ticks afterwards find no press: the same K never starts twice.
	TickN(*Component, 2);
	TestTrue(TEXT("ticking on stays Free"), Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("no second launcher start from the same K"), Events.StartedCount, 1);
	TestEqual(TEXT("the buffer stays empty after the natural end"), Component->GetSnapshot().BufferSize, 0);
	return true;
}

// Death refuses everything: a dead component neither starts from a buffered K
// (without consuming it) nor switches inside a cancel window while dead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021DeadComponentRefusesLauncherStartAndSwitch,
	"UEMMO.Tasks.M1_021.DeadComponentRefusesLauncherStartAndSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021DeadComponentRefusesLauncherStartAndSwitch::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	// Part A: a dead Free component ignores a valid buffered K entirely.
	Component->SetDead(true);
	Component->SetInputClockSeconds(1.0);
	QueueAction(*Component, 1, ECombatInput::Launcher, 1.0);
	Component->TickCombat(static_cast<float>(FrameSeconds));

	TestTrue(TEXT("the component is dead"), Component->IsDead());
	TestTrue(TEXT("the dead component stays Free (death is a separate flag)"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the dead component consumes nothing"), Component->GetSnapshot().BufferSize, 1);
	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the buffered press is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestTrue(TEXT("the buffered press is the Launcher"), Buffered.Action == ECombatInput::Launcher);
	TestEqual(TEXT("the dead component minted no instance"), Component->GetSnapshot().InstanceId, uint64(0));
	TestEqual(TEXT("the dead component fired no Started"), Events.StartedCount, 0);

	// Part B: a dead running attack refuses the cancel-window switch without
	// consuming the press. Revive and reset for a clean session first.
	Component->SetDead(false);
	Component->ResetCombat();
	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TickN(*Component, 12);
	TestEqual(TEXT("precondition: the attack is at Frame 11"), Component->GetSnapshot().Frame, 11);
	Component->SetInputClockSeconds(11.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 11.0 * FrameSeconds);
	Component->SetDead(true);

	// Frame 12 opens the window; the dead component must not switch or consume.
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Refused = Component->GetSnapshot();
	TestEqual(TEXT("the dead running attack does not switch (still light_01)"), Refused.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("the dead running attack keeps InstanceId 1"), Refused.InstanceId, uint64(1));
	TestEqual(TEXT("the dead running attack keeps the press buffered"), Refused.BufferSize, 1);
	TestEqual(TEXT("the refused switch fired no Finished"), Events.FinishedCount, 0);
	TestEqual(TEXT("the refused switch fired no extra Started"), Events.StartedCount, 1);
	return true;
}

// A Launcher press inside launcher's own cancel window is not consumed: the
// launcher attack allows no launcher follow-up (its next list holds no
// launcher), so the press survives every window step and the natural end.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_021LauncherWindowKeepsLauncherPressAndEndsNaturally,
	"UEMMO.Tasks.M1_021.LauncherWindowKeepsLauncherPressAndEndsNaturally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_021LauncherWindowKeepsLauncherPressAndEndsNaturally::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FTransitionEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	// Advance to Frame 18 where launcher's cancel window [18,32) opens and
	// press K there; the input clock then only moves 1/60 s per window step,
	// so the press never ages out and the keep-behavior is isolated from the
	// expiry rule.
	TickN(*Component, 19);
	TestEqual(TEXT("precondition: launcher is at Frame 18"), Component->GetSnapshot().Frame, 18);
	Component->SetInputClockSeconds(18.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Launcher, 18.0 * FrameSeconds);
	Component->SetInputClockSeconds(19.0 * FrameSeconds);

	// First window step: a Launcher maps to launcher, which launcher's
	// AllowedNextAttacks do not contain, so nothing is consumed.
	TickN(*Component, 1);
	const FCombatSnapshot WindowStep = Component->GetSnapshot();
	TestEqual(TEXT("the K press does not switch (still launcher)"), WindowStep.AttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("the K press keeps the running InstanceId 1"), WindowStep.InstanceId, uint64(1));
	TestEqual(TEXT("the K press survives the first window step"), WindowStep.BufferSize, 1);
	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the buffered press is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestTrue(TEXT("the buffered press is the Launcher"), Buffered.Action == ECombatInput::Launcher);
	TestEqual(TEXT("the buffered Launcher keeps Sequence 1"), Buffered.Sequence, uint64(1));
	TestEqual(TEXT("no Finished fired yet"), Events.FinishedCount, 0);

	// Run through the rest of the window and the natural end: no step consumes
	// the press and nothing switches.
	TickN(*Component, 20);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("launcher ends naturally back to Free"), Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("Finished carries the naturally ended launcher"), Events.FinishedAttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("no switch fired a second Started"), Events.StartedCount, 1);
	TestEqual(TEXT("the K press is still buffered after the natural end"), Done.BufferSize, 1);
	TestTrue(TEXT("the surviving entry is still the Launcher"),
		Component->PeekInputBuffer(Buffered, 0) && Buffered.Action == ECombatInput::Launcher);
	return true;
}

#endif
