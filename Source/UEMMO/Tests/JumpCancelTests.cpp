#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatInputBuffer.h"
#include "../PrototypeCharacter.h"

#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_023
{
	// The tests drive the component with single 1/60 s steps, so logic frame N
	// sits at N/60 s of game time. The input clock is injected explicitly
	// (interface contract section 2); TickCombat never advances it itself.
	constexpr double FrameSeconds = 1.0 / 60.0;

	// Builds a catalog from Config/DefaultGame.ini (the four explicit DA
	// references, the same loading path the M1-010/M1-011/M1-021 tests use)
	// and injects it into a fresh combat component. Returns null after
	// reporting the failure so the caller can bail out early.
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

	// Counts Started/Finished broadcasts and the owner jump requests. The jump
	// request is the injectable M1-023 handler: the component cannot jump
	// itself, so a bound counting double captures exactly how often the
	// component asked its owner to perform the real ACharacter::Jump.
	struct FJumpCancelEvents
	{
		int32 StartedCount = 0;
		int32 FinishedCount = 0;
		int32 JumpRequests = 0;
		FName FinishedAttackId = NAME_None;

		void Bind(UCombatComponent& Component)
		{
			Component.OnStarted.AddLambda([this](FName, uint64)
			{
				++StartedCount;
			});
			Component.OnFinished.AddLambda([this](FName AttackId, uint64)
			{
				++FinishedCount;
				FinishedAttackId = AttackId;
			});
			Component.SetJumpRequestHandler([this]()
			{
				++JumpRequests;
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

	// World acquisition, same order as the M1-018/M1-022 pattern: a private
	// temp world first, the shared game world as fallback.
	static UWorld* AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_023_TestWorld")));
		if (TempWorld != nullptr)
		{
			return TempWorld;
		}
		return GWorld;
	}

	// Spawns the real prototype character in the acquired world and dispatches
	// BeginPlay exactly once (temp worlds may not have begun play; the M1-022
	// pattern). BeginPlay owns the catalog attachment and the M1-023 jump
	// request binding, so without it the character-side routing stays inert.
	static APrototypeCharacter* SpawnPrototypeCharacter(UWorld& World, FAutomationTestBase& Test)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Character = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Test.TestNotNull(TEXT("the prototype character spawns"), Character))
		{
			return nullptr;
		}
		if (!Character->HasActorBegunPlay())
		{
			Character->DispatchBeginPlay();
		}
		return Character;
	}
}

using namespace UE::UEMMO::Tasks::M1_023;

// A Jump pressed at Frame 17 buffers into the launcher cancel window and the
// Frame 18 step (the first inside [18,32), read from the definition) cancels
// the attack: the snapshot returns to Free with no Finished broadcast (a
// cancel is an interruption, not a timeline end) and the owner received
// exactly one jump request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023LauncherFrame18JumpCancelsAttackAndRequestsJumpOnce,
	"UEMMO.Tasks.M1_023.LauncherFrame18JumpCancelsAttackAndRequestsJumpOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023LauncherFrame18JumpCancelsAttackAndRequestsJumpOnce::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	// Advance to Frame 17 and press Space there (PressedAt = 17/60 s).
	TickN(*Component, 18);
	TestEqual(TEXT("precondition: the attack is at Frame 17"), Component->GetSnapshot().Frame, 17);
	Component->SetInputClockSeconds(17.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 17.0 * FrameSeconds);
	TestEqual(TEXT("the Frame 17 press is buffered"), Component->GetSnapshot().BufferSize, 1);

	// The Frame 18 step opens the cancel window [18,32); the intent age is
	// 1/60 s, well inside the 150 ms lifetime, so the cancel fires here.
	TickN(*Component, 1);

	const FCombatSnapshot Cancelled = Component->GetSnapshot();
	TestTrue(TEXT("the jump cancel returns the snapshot to Free"),
		Cancelled.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the cancelled instance clears its InstanceId"), Cancelled.InstanceId, uint64(0));
	TestEqual(TEXT("the cancelled instance clears its AttackId"), Cancelled.AttackId, FName(NAME_None));
	TestEqual(TEXT("the cancelled instance clears its Frame"), Cancelled.Frame, -1);
	TestEqual(TEXT("the consumed intent empties the buffer"), Cancelled.BufferSize, 0);

	TestEqual(TEXT("the cancel never broadcasts Finished (cancel is not a finish)"), Events.FinishedCount, 0);
	TestEqual(TEXT("the owner received exactly one jump request"), Events.JumpRequests, 1);
	TestEqual(TEXT("no extra attack started (a jump cancel starts no attack)"), Events.StartedCount, 1);
	return true;
}

// A Jump pressed at Frame 16 does not cancel on the Frame 17 step: Frame 17 is
// one frame before the [18,32) cancel window, so the attack keeps running and
// the intent stays buffered for the window opening.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023LauncherFrame17JumpDoesNotCancelAndStaysBuffered,
	"UEMMO.Tasks.M1_023.LauncherFrame17JumpDoesNotCancelAndStaysBuffered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023LauncherFrame17JumpDoesNotCancelAndStaysBuffered::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	TickN(*Component, 17);
	TestEqual(TEXT("precondition: the attack is at Frame 16"), Component->GetSnapshot().Frame, 16);
	Component->SetInputClockSeconds(16.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 16.0 * FrameSeconds);

	// The Frame 17 step sits one frame before the window: nothing consumes.
	TickN(*Component, 1);

	const FCombatSnapshot BeforeWindow = Component->GetSnapshot();
	TestTrue(TEXT("Frame 17 does not cancel the attack"),
		BeforeWindow.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the running instance is untouched at Frame 17"), BeforeWindow.InstanceId, uint64(1));
	TestEqual(TEXT("the attack is at Frame 17"), BeforeWindow.Frame, 17);
	TestEqual(TEXT("the intent stays buffered before the window"), BeforeWindow.BufferSize, 1);
	TestEqual(TEXT("no jump request fired at Frame 17"), Events.JumpRequests, 0);
	TestEqual(TEXT("no Finished fired at Frame 17"), Events.FinishedCount, 0);

	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the buffered press is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestTrue(TEXT("the buffered press is the Jump"), Buffered.Action == ECombatInput::Jump);
	return true;
}

// A Jump pressed at Frame 30 cancels on the Frame 31 step: the last frame of
// the [18,32) cancel window still allows the jump cancel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023LauncherFrame31JumpStillCancels,
	"UEMMO.Tasks.M1_023.LauncherFrame31JumpStillCancels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023LauncherFrame31JumpStillCancels::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	TickN(*Component, 31);
	TestEqual(TEXT("precondition: the attack is at Frame 30"), Component->GetSnapshot().Frame, 30);
	Component->SetInputClockSeconds(30.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 30.0 * FrameSeconds);

	// Frame 31 is inside [18,32): the cancel fires.
	TickN(*Component, 1);

	const FCombatSnapshot Cancelled = Component->GetSnapshot();
	TestTrue(TEXT("the Frame 31 jump cancel returns the snapshot to Free"),
		Cancelled.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the cancelled instance cleared its InstanceId"), Cancelled.InstanceId, uint64(0));
	TestEqual(TEXT("the consumed intent empties the buffer"), Cancelled.BufferSize, 0);
	TestEqual(TEXT("the cancel never broadcasts Finished"), Events.FinishedCount, 0);
	TestEqual(TEXT("the owner received exactly one jump request"), Events.JumpRequests, 1);
	return true;
}

// A Jump pressed at Frame 31 does not cancel on the Frame 32 step: the
// [18,32) window is half-open, so Frame 32 is outside. The attack runs out
// naturally, the expired intent is pruned (never consumed) and no jump
// request ever fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023LauncherFrame32JumpDoesNotCancelAndExpires,
	"UEMMO.Tasks.M1_023.LauncherFrame32JumpDoesNotCancelAndExpires",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023LauncherFrame32JumpDoesNotCancelAndExpires::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	TickN(*Component, 32);
	TestEqual(TEXT("precondition: the attack is at Frame 31"), Component->GetSnapshot().Frame, 31);
	Component->SetInputClockSeconds(31.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 31.0 * FrameSeconds);

	// Frame 32 is the first frame after the window: no cancel, intent kept.
	TickN(*Component, 1);

	const FCombatSnapshot AfterWindow = Component->GetSnapshot();
	TestTrue(TEXT("Frame 32 does not cancel the attack"),
		AfterWindow.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the running instance is untouched at Frame 32"), AfterWindow.InstanceId, uint64(1));
	TestEqual(TEXT("the attack is at Frame 32"), AfterWindow.Frame, 32);
	TestEqual(TEXT("no jump request fired at Frame 32"), Events.JumpRequests, 0);

	// Age the intent past its 150 ms lifetime, then run the attack out: it
	// ends naturally at Frame 39 (duration 40) with exactly one Finished.
	Component->SetInputClockSeconds(31.0 * FrameSeconds + 0.2);
	TickN(*Component, 7);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("the launcher ends naturally back to Free"),
		Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("no jump request fired for the expired intent"), Events.JumpRequests, 0);
	TestEqual(TEXT("no extra attack started"), Events.StartedCount, 1);

	// The first Free tick prunes the expired intent instead of consuming it.
	TickN(*Component, 1);
	TestEqual(TEXT("the expired intent was pruned, not consumed"), Component->GetSnapshot().BufferSize, 0);
	TestEqual(TEXT("the expired intent never requested a jump"), Events.JumpRequests, 0);
	return true;
}

// A Jump pressed at Frame 10 (well before the window) survives the pre-window
// frames and cancels on the Frame 18 step: its age there is 8/60 s (~133 ms),
// inside the 150 ms lifetime, so the early press buffers into the window.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023EarlyPressInsideLifetimeBuffersIntoWindowAndCancels,
	"UEMMO.Tasks.M1_023.EarlyPressInsideLifetimeBuffersIntoWindowAndCancels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023EarlyPressInsideLifetimeBuffersIntoWindowAndCancels::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	// Press at Frame 10 with the input clock following the frame grid.
	TickN(*Component, 11);
	TestEqual(TEXT("precondition: the attack is at Frame 10"), Component->GetSnapshot().Frame, 10);
	Component->SetInputClockSeconds(10.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 10.0 * FrameSeconds);

	// Frames 11..17 stay outside the window: the press waits, the attack runs.
	for (int32 Frame = 11; Frame <= 17; ++Frame)
	{
		Component->SetInputClockSeconds(static_cast<double>(Frame) * FrameSeconds);
		TickN(*Component, 1);
		const FCombatSnapshot Waiting = Component->GetSnapshot();
		TestTrue(FString::Printf(TEXT("Frame %d keeps the attack running"), Frame),
			Waiting.ActionState == ECombatActionState::Attacking);
		TestEqual(FString::Printf(TEXT("Frame %d keeps the early press buffered"), Frame),
			Waiting.BufferSize, 1);
		TestEqual(FString::Printf(TEXT("Frame %d requests no jump"), Frame), Events.JumpRequests, 0);
	}

	// Frame 18 opens the window; the press age is 8/60 s, inside the lifetime.
	Component->SetInputClockSeconds(18.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot Cancelled = Component->GetSnapshot();
	TestTrue(TEXT("the buffered early press cancels the attack at Frame 18"),
		Cancelled.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the cancelled instance cleared its InstanceId"), Cancelled.InstanceId, uint64(0));
	TestEqual(TEXT("the consumed early press empties the buffer"), Cancelled.BufferSize, 0);
	TestEqual(TEXT("the cancel never broadcasts Finished"), Events.FinishedCount, 0);
	TestEqual(TEXT("the owner received exactly one jump request"), Events.JumpRequests, 1);
	return true;
}

// A Jump pressed at Frame 1 is older than the 150 ms lifetime when the cancel
// window opens (age 17/60 s ~ 283 ms): the first window step prunes it without
// consuming, the launcher ends naturally and no jump request ever fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023EarlyPressExpiredDoesNotCancelAndLauncherEndsNaturally,
	"UEMMO.Tasks.M1_023.EarlyPressExpiredDoesNotCancelAndLauncherEndsNaturally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023EarlyPressExpiredDoesNotCancelAndLauncherEndsNaturally::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	TickN(*Component, 2);
	TestEqual(TEXT("precondition: the attack is at Frame 1"), Component->GetSnapshot().Frame, 1);
	Component->SetInputClockSeconds(1.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 1.0 * FrameSeconds);

	// Frames 2..17 keep the press (outside the window nothing prunes); at
	// Frame 18 the first window step drops the expired press untouched.
	for (int32 Frame = 2; Frame <= 17; ++Frame)
	{
		Component->SetInputClockSeconds(static_cast<double>(Frame) * FrameSeconds);
		TickN(*Component, 1);
		TestEqual(FString::Printf(TEXT("Frame %d keeps the press buffered"), Frame),
			Component->GetSnapshot().BufferSize, 1);
	}
	Component->SetInputClockSeconds(18.0 * FrameSeconds);
	TickN(*Component, 1);

	const FCombatSnapshot WindowStep = Component->GetSnapshot();
	TestTrue(TEXT("the expired press does not cancel (still launcher)"),
		WindowStep.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the expired press was pruned, not consumed"), WindowStep.BufferSize, 0);
	TestEqual(TEXT("the first window step lands on Frame 18"), WindowStep.Frame, 18);
	TestEqual(TEXT("the expired press requested no jump"), Events.JumpRequests, 0);

	// Run out the remaining frames 19..39: the launcher ends naturally.
	Component->SetInputClockSeconds(39.0 * FrameSeconds);
	TickN(*Component, 21);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("the launcher ends naturally back to Free"),
		Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("the natural end carries the launcher"), Events.FinishedAttackId, FName(TEXT("launcher")));
	TestEqual(TEXT("no jump request ever fired"), Events.JumpRequests, 0);
	TestEqual(TEXT("no extra attack started"), Events.StartedCount, 1);

	TickN(*Component, 1);
	TestEqual(TEXT("the buffer stays empty after the natural end"),
		Component->GetSnapshot().BufferSize, 0);
	return true;
}

// After the Frame 18 jump cancel the consumed Sequence is gone: further ticks
// neither cancel again nor request another jump, and no attack starts from the
// leftover state (one press, one cancel, one jump).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023ConsumedJumpSequenceNeverCancelsOrJumpsTwice,
	"UEMMO.Tasks.M1_023.ConsumedJumpSequenceNeverCancelsOrJumpsTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023ConsumedJumpSequenceNeverCancelsOrJumpsTwice::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	TickN(*Component, 18);
	Component->SetInputClockSeconds(17.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 17.0 * FrameSeconds);
	TickN(*Component, 1);
	TestEqual(TEXT("precondition: the Frame 18 step cancelled into one jump request"),
		Events.JumpRequests, 1);
	TestEqual(TEXT("precondition: the buffer is empty after the consume"),
		Component->GetSnapshot().BufferSize, 0);

	// Six further advancing frames: no second cancel, no second jump request,
	// no attack start, no Finished out of nowhere.
	for (int32 Frame = 19; Frame <= 24; ++Frame)
	{
		Component->SetInputClockSeconds(static_cast<double>(Frame) * FrameSeconds);
		TickN(*Component, 1);
		TestTrue(FString::Printf(TEXT("Frame %d stays Free after the cancel"), Frame),
			Component->GetSnapshot().ActionState == ECombatActionState::Free);
		TestEqual(FString::Printf(TEXT("Frame %d keeps exactly one jump request"), Frame),
			Events.JumpRequests, 1);
	}
	TestEqual(TEXT("no extra Started fired after the cancel"), Events.StartedCount, 1);
	TestEqual(TEXT("no Finished fired after the cancel"), Events.FinishedCount, 0);
	TestEqual(TEXT("the buffer stays empty"), Component->GetSnapshot().BufferSize, 0);
	return true;
}

// A Jump intent buffered while Free requests the owner jump exactly once on
// the next combat tick and starts no attack (the M0 free jump, routed through
// the buffer). A further tick consumes nothing again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023FreeJumpIntentRequestsJumpOnce,
	"UEMMO.Tasks.M1_023.FreeJumpIntentRequestsJumpOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023FreeJumpIntentRequestsJumpOnce::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	Component->SetInputClockSeconds(1.0);
	QueueAction(*Component, 1, ECombatInput::Jump, 1.0);
	TestEqual(TEXT("the Free jump intent is buffered"), Component->GetSnapshot().BufferSize, 1);

	TickN(*Component, 1);

	const FCombatSnapshot After = Component->GetSnapshot();
	TestTrue(TEXT("the consumed Free jump leaves the component Free"),
		After.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the consumed intent empties the buffer"), After.BufferSize, 0);
	TestEqual(TEXT("the owner received exactly one jump request"), Events.JumpRequests, 1);
	TestEqual(TEXT("a Free jump starts no attack"), Events.StartedCount, 0);
	TestEqual(TEXT("a Free jump fires no Finished"), Events.FinishedCount, 0);

	TickN(*Component, 1);
	TestEqual(TEXT("a further tick consumes nothing (no second request)"),
		Events.JumpRequests, 1);
	return true;
}

// The game path binds the jump handler without injecting the input clock (the
// per-frame clock injection still belongs to a later wiring task): the Free
// jump consumption is gated on the bound handler, not on the clock, so Space
// keeps working in Free without a clock. The consumed press removes the entry
// exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023FreeJumpWithoutInjectedClockStillRequestsJump,
	"UEMMO.Tasks.M1_023.FreeJumpWithoutInjectedClockStillRequestsJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023FreeJumpWithoutInjectedClockStillRequestsJump::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	// No SetInputClockSeconds call: the component has no injected "now". The
	// press carries a positive world-style time like the game's queue path.
	QueueAction(*Component, 1, ECombatInput::Jump, 1.0);
	TickN(*Component, 1);

	const FCombatSnapshot After = Component->GetSnapshot();
	TestTrue(TEXT("the clock-less Free jump stays Free"),
		After.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the clock-less Free jump consumed the intent"), After.BufferSize, 0);
	TestEqual(TEXT("the owner received exactly one jump request"), Events.JumpRequests, 1);
	TestEqual(TEXT("no attack started"), Events.StartedCount, 0);

	TickN(*Component, 1);
	TestEqual(TEXT("a further tick consumes nothing (no second request)"),
		Events.JumpRequests, 1);
	return true;
}

// Without a bound jump handler there is nothing to cancel into: a Jump inside
// the launcher cancel window stays buffered and never cancels the attack (the
// gate that keeps the pre-M1-023 observation-only behavior for bare
// components verbatim).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023WindowJumpWithoutHandlerStaysBuffered,
	"UEMMO.Tasks.M1_023.WindowJumpWithoutHandlerStaysBuffered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023WindowJumpWithoutHandlerStaysBuffered::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);
	// Undo the handler the events helper bound: this test owns the unbound
	// gate. (Passing an empty function unbinds per the M1-023 contract.)
	Component->SetJumpRequestHandler(FCombatJumpRequestHandler());

	TestTrue(TEXT("precondition: launcher starts as instance 1"), Component->TryStartAttack(FName(TEXT("launcher")), 1));

	TickN(*Component, 18);
	Component->SetInputClockSeconds(17.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 17.0 * FrameSeconds);

	// The Frame 18 step would cancel with a handler; unbound it must not.
	TickN(*Component, 1);

	const FCombatSnapshot Kept = Component->GetSnapshot();
	TestTrue(TEXT("an unbound handler keeps the attack running"),
		Kept.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the unbound window intent stays buffered"), Kept.BufferSize, 1);
	TestEqual(TEXT("no jump request can fire without a handler"), Events.JumpRequests, 0);
	TestEqual(TEXT("no Finished fired without a handler"), Events.FinishedCount, 0);

	FBufferedCombatInput Buffered;
	TestTrue(TEXT("the kept intent is readable"), Component->PeekInputBuffer(Buffered, 0));
	TestTrue(TEXT("the kept intent is the Jump"), Buffered.Action == ECombatInput::Jump);
	return true;
}

// A Jump inside light_01's own cancel window [12,24) is not consumed: the jump
// cancel is launcher-only (input semantics, not an AllowedNextAttacks entry).
// The attack ends naturally, the aged intent is pruned and no jump fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023Light01WindowJumpIsKeptNotConsumed,
	"UEMMO.Tasks.M1_023.Light01WindowJumpIsKeptNotConsumed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023Light01WindowJumpIsKeptNotConsumed::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FJumpCancelEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	TickN(*Component, 13);
	TestEqual(TEXT("precondition: the attack is at Frame 12"), Component->GetSnapshot().Frame, 12);
	Component->SetInputClockSeconds(12.0 * FrameSeconds);
	QueueAction(*Component, 1, ECombatInput::Jump, 12.0 * FrameSeconds);

	// First light_01 window step: the Jump is kept, not consumed.
	TickN(*Component, 1);

	const FCombatSnapshot Kept = Component->GetSnapshot();
	TestTrue(TEXT("the light_01 window does not cancel into a jump"),
		Kept.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the running instance is untouched"), Kept.InstanceId, uint64(1));
	TestEqual(TEXT("the light-window Jump stays buffered"), Kept.BufferSize, 1);
	TestEqual(TEXT("no jump request fired in the light_01 window"), Events.JumpRequests, 0);

	// Age the press past its lifetime and run light_01 out: it ends naturally
	// at Frame 25 (duration 26) with the press pruned on the first Free tick.
	Component->SetInputClockSeconds(12.0 * FrameSeconds + 0.2);
	TickN(*Component, 13);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("light_01 ends naturally back to Free"),
		Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the natural end broadcasts Finished exactly once"), Events.FinishedCount, 1);
	TestEqual(TEXT("the natural end carries light_01"), Events.FinishedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("no jump request ever fired"), Events.JumpRequests, 0);

	TickN(*Component, 1);
	TestEqual(TEXT("the aged light-window Jump was pruned, not consumed"),
		Component->GetSnapshot().BufferSize, 0);
	return true;
}

// Character-level routing: the real prototype character's Space binding
// (StartJump) enqueues a Jump intent instead of bypassing the state, and the
// Free-state consumption on the next combat tick performs the owner jump
// through the BeginPlay-bound handler. The buffer empties exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023CharacterSpaceFreeBuffersThenJumpsOnNextCombatTick,
	"UEMMO.Tasks.M1_023.CharacterSpaceFreeBuffersThenJumpsOnNextCombatTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023CharacterSpaceFreeBuffersThenJumpsOnNextCombatTick::RunTest(const FString& Parameters)
{
	UWorld* World = AcquireWorld();
	if (!TestNotNull(TEXT("a world is available for the character test"), World))
	{
		return true;
	}
	APrototypeCharacter* Character = SpawnPrototypeCharacter(*World, *this);
	if (Character == nullptr)
	{
		return true;
	}
	UCombatComponent* Combat = Character->GetCombat();
	if (!TestNotNull(TEXT("the character owns a combat component"), Combat))
	{
		return true;
	}

	Character->StartJump();

	const FCombatSnapshot Buffered = Combat->GetSnapshot();
	TestEqual(TEXT("Space buffers exactly one intent"), Buffered.BufferSize, 1);
	FBufferedCombatInput Entry;
	TestTrue(TEXT("the buffered intent is readable"), Combat->PeekInputBuffer(Entry, 0));
	TestTrue(TEXT("the buffered intent is the Jump"), Entry.Action == ECombatInput::Jump);

	// The next combat tick consumes the Free intent into the owner jump: the
	// handler is the only Free consumer, so the emptied buffer is the
	// observable of the performed ACharacter::Jump.
	Combat->TickCombat(static_cast<float>(FrameSeconds));

	const FCombatSnapshot After = Combat->GetSnapshot();
	TestTrue(TEXT("the Free jump leaves the character Free"),
		After.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the Free jump consumed the intent"), After.BufferSize, 0);

	Combat->TickCombat(static_cast<float>(FrameSeconds));
	TestEqual(TEXT("a further combat tick consumes nothing"),
		Combat->GetSnapshot().BufferSize, 0);
	return true;
}

// Character-level jump cancel: with the launcher running inside its cancel
// window, the character's Space binding buffers the intent and the next
// combat tick cancels the attack (no Finished) instead of bypassing the state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_023CharacterSpaceDuringLauncherWindowCancelsIntoJump,
	"UEMMO.Tasks.M1_023.CharacterSpaceDuringLauncherWindowCancelsIntoJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_023CharacterSpaceDuringLauncherWindowCancelsIntoJump::RunTest(const FString& Parameters)
{
	UWorld* World = AcquireWorld();
	if (!TestNotNull(TEXT("a world is available for the character test"), World))
	{
		return true;
	}
	APrototypeCharacter* Character = SpawnPrototypeCharacter(*World, *this);
	if (Character == nullptr)
	{
		return true;
	}
	UCombatComponent* Combat = Character->GetCombat();
	if (!TestNotNull(TEXT("the character owns a combat component"), Combat))
	{
		return true;
	}

	int32 FinishedCount = 0;
	Combat->OnFinished.AddLambda([&FinishedCount](FName, uint64)
	{
		++FinishedCount;
	});

	// Direct start: the game-side start wiring (clock injection, facing) is a
	// later task; this test owns the cancel path only.
	TestTrue(TEXT("precondition: the launcher starts"), Combat->TryStartAttack(FName(TEXT("launcher")), 1));
	for (int32 Index = 0; Index < 18; ++Index)
	{
		Combat->TickCombat(static_cast<float>(FrameSeconds));
	}
	TestEqual(TEXT("precondition: the attack is at Frame 17"), Combat->GetSnapshot().Frame, 17);

	Character->StartJump();
	TestEqual(TEXT("Space during the attack buffers the intent instead of jumping directly"),
		Combat->GetSnapshot().BufferSize, 1);

	// The Frame 18 step cancels the running launcher through the buffered
	// Space press (the temp world clock sits at 0.0, so the press age is 0).
	Combat->TickCombat(static_cast<float>(FrameSeconds));

	const FCombatSnapshot Cancelled = Combat->GetSnapshot();
	TestTrue(TEXT("the jump cancel returned the character to Free"),
		Cancelled.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the cancelled instance cleared its InstanceId"), Cancelled.InstanceId, uint64(0));
	TestEqual(TEXT("the consumed intent emptied the buffer"), Cancelled.BufferSize, 0);
	TestEqual(TEXT("the cancel never broadcast Finished"), FinishedCount, 0);

	Combat->TickCombat(static_cast<float>(FrameSeconds));
	TestTrue(TEXT("ticking on after the cancel stays Free"),
		Combat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the cancelled Sequence never cancels twice"), Combat->GetSnapshot().BufferSize, 0);
	return true;
}

#endif
