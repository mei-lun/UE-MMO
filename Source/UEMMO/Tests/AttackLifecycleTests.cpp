#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_011
{
	// Builds a catalog from Config/DefaultGame.ini (the four explicit DA
	// references, the same loading path the M1-010 catalog tests use) and
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
	// can assert exactly-once semantics and the ids carried by the events.
	struct FLifecycleEvents
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
			Component.TickCombat(1.0f / 60.0f);
		}
	}
}

using namespace UE::UEMMO::Tasks::M1_011;

// An idle component starts light_01, mints InstanceId 1 with Frame still -1
// and carries AttackId/Facing/BufferSize in the snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011IdleCanStartAndSnapshotCarriesInstance,
	"UEMMO.Tasks.M1_011.IdleCanStartAndSnapshotCarriesInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011IdleCanStartAndSnapshotCarriesInstance::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FLifecycleEvents Events;
	Events.Bind(*Component);

	const FCombatSnapshot Idle = Component->GetSnapshot();
	TestTrue(TEXT("an idle component reports ActionState Free"), Idle.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("an idle component reports InstanceId 0"), Idle.InstanceId, uint64(0));
	TestEqual(TEXT("an idle component reports Frame -1"), Idle.Frame, -1);
	TestEqual(TEXT("an idle component reports no AttackId"), Idle.AttackId, FName(NAME_None));
	TestEqual(TEXT("the M1-011 input buffer starts empty"), Idle.BufferSize, 0);
	TestTrue(TEXT("an idle component accepts movement"), Component->CanAcceptMovement());
	TestTrue(TEXT("an idle component accepts turning"), Component->CanTurn());

	TestTrue(TEXT("TryStartAttack('light_01', 1) succeeds from Free with the id in the catalog"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));

	const FCombatSnapshot Started = Component->GetSnapshot();
	TestTrue(TEXT("snapshot ActionState is Attacking right after the start"),
		Started.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the first successful start mints InstanceId 1"), Started.InstanceId, uint64(1));
	TestEqual(TEXT("snapshot Frame stays -1 until the first tick"), Started.Frame, -1);
	TestEqual(TEXT("snapshot AttackId carries the started attack"), Started.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("snapshot Facing carries the requested facing"), Started.Facing, 1);
	TestEqual(TEXT("the M1-011 input buffer stays empty while attacking"), Started.BufferSize, 0);
	TestFalse(TEXT("an attacking component refuses movement"), Component->CanAcceptMovement());
	TestFalse(TEXT("an attacking component refuses turning"), Component->CanTurn());

	TestEqual(TEXT("exactly one Started broadcast fired"), Events.StartedCount, 1);
	TestEqual(TEXT("Started carries the started AttackId"), Events.StartedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("Started carries the new InstanceId"), Events.StartedInstanceId, uint64(1));
	TestEqual(TEXT("no Finished broadcast fired at start"), Events.FinishedCount, 0);
	return true;
}

// The first 1/60 s step after the start lands on Frame 0, intermediate steps
// keep the attack running, and the 26th step (DurationFrames - 1 = 25) ends
// light_01 with exactly one Finished broadcast.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011FirstStepFrameZeroAndFinishesAfter26Steps,
	"UEMMO.Tasks.M1_011.FirstStepFrameZeroAndFinishesAfter26Steps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011FirstStepFrameZeroAndFinishesAfter26Steps::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FLifecycleEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	TickN(*Component, 1);
	const FCombatSnapshot FirstStep = Component->GetSnapshot();
	TestEqual(TEXT("the first step after the start lands on Frame 0"), FirstStep.Frame, 0);
	TestTrue(TEXT("the attack is still running after the first step"),
		FirstStep.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("no Finished broadcast after the first step"), Events.FinishedCount, 0);

	// light_01 covers DurationFrames = 26 logic frames 0..25; the attack must
	// still be running through step 25 (Frame 24).
	for (int32 Step = 2; Step <= 25; ++Step)
	{
		TickN(*Component, 1);
		const FCombatSnapshot Mid = Component->GetSnapshot();
		TestTrue(FString::Printf(TEXT("step %d of 26 is still inside the attack"), Step),
			Mid.ActionState == ECombatActionState::Attacking);
		TestEqual(TEXT("no Finished broadcast before the last frame"), Events.FinishedCount, 0);
	}

	TickN(*Component, 1);
	const FCombatSnapshot Done = Component->GetSnapshot();
	TestTrue(TEXT("the 26th step ends the attack back to Free"),
		Done.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the finished instance clears its Frame to -1"), Done.Frame, -1);
	TestEqual(TEXT("the finished instance clears its InstanceId"), Done.InstanceId, uint64(0));
	TestEqual(TEXT("the finished instance clears its AttackId"), Done.AttackId, FName(NAME_None));
	TestEqual(TEXT("OnFinished is broadcast exactly once for the completed instance"), Events.FinishedCount, 1);
	TestEqual(TEXT("OnFinished carries the finished AttackId"), Events.FinishedAttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("OnFinished carries the finished InstanceId"), Events.FinishedInstanceId, uint64(1));
	TestEqual(TEXT("OnStarted still fired exactly once"), Events.StartedCount, 1);
	return true;
}

// An in-flight attack rejects unconditional reentry (same id or another id),
// and after it ends a new start succeeds with a fresh InstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011ReentryRejectedAndRestartMintsNewInstanceId,
	"UEMMO.Tasks.M1_011.ReentryRejectedAndRestartMintsNewInstanceId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011ReentryRejectedAndRestartMintsNewInstanceId::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FLifecycleEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	TestFalse(TEXT("restarting the same id while attacking is rejected"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TestFalse(TEXT("starting a different id while attacking is rejected"),
		Component->TryStartAttack(FName(TEXT("launcher")), -1));
	const FCombatSnapshot Unchanged = Component->GetSnapshot();
	TestEqual(TEXT("rejected reentry keeps the running AttackId"), Unchanged.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("rejected reentry keeps the running InstanceId"), Unchanged.InstanceId, uint64(1));
	TestEqual(TEXT("rejected reentry keeps the running Facing"), Unchanged.Facing, 1);
	TestEqual(TEXT("rejected reentry fires no extra Started"), Events.StartedCount, 1);

	TickN(*Component, 26);
	TestTrue(TEXT("precondition: the attack ended back to Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the finished instance broadcast Finished once"), Events.FinishedCount, 1);

	TestTrue(TEXT("a new start succeeds after the attack ended"),
		Component->TryStartAttack(FName(TEXT("light_01")), -1));
	const FCombatSnapshot Restarted = Component->GetSnapshot();
	TestEqual(TEXT("the restart mints a fresh InstanceId 2"), Restarted.InstanceId, uint64(2));
	TestEqual(TEXT("the restart carries the new Facing"), Restarted.Facing, -1);
	TestEqual(TEXT("the restart fired a second Started"), Events.StartedCount, 2);
	TestEqual(TEXT("the second Started carries InstanceId 2"), Events.StartedInstanceId, uint64(2));
	return true;
}

// ResetCombat mid-attack returns to Free immediately without broadcasting
// Finished, and the next start mints a different InstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011ResetMidAttackFreesImmediatelyWithoutFinished,
	"UEMMO.Tasks.M1_011.ResetMidAttackFreesImmediatelyWithoutFinished",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011ResetMidAttackFreesImmediatelyWithoutFinished::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FLifecycleEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts as instance 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TickN(*Component, 5);
	TestEqual(TEXT("precondition: the attack is mid-timeline at Frame 4"), Component->GetSnapshot().Frame, 4);

	Component->ResetCombat();

	const FCombatSnapshot Reset = Component->GetSnapshot();
	TestTrue(TEXT("ResetCombat returns to Free immediately"), Reset.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("ResetCombat clears Frame to -1"), Reset.Frame, -1);
	TestEqual(TEXT("ResetCombat clears AttackId"), Reset.AttackId, FName(NAME_None));
	TestEqual(TEXT("ResetCombat clears InstanceId"), Reset.InstanceId, uint64(0));
	TestEqual(TEXT("ResetCombat clears Facing"), Reset.Facing, 0);
	TestEqual(TEXT("ResetCombat clears the input buffer"), Reset.BufferSize, 0);
	TestEqual(TEXT("ResetCombat never broadcasts Finished"), Events.FinishedCount, 0);
	TestEqual(TEXT("the Started broadcast of the reset instance is kept"), Events.StartedCount, 1);

	TickN(*Component, 3);
	TestTrue(TEXT("ticking after a reset stays Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("ticking after a reset still fires no Finished"), Events.FinishedCount, 0);

	TestTrue(TEXT("a new start succeeds after the reset"),
		Component->TryStartAttack(FName(TEXT("light_01")), -1));
	TestEqual(TEXT("the post-reset start mints a different InstanceId"), Component->GetSnapshot().InstanceId, uint64(2));
	TestEqual(TEXT("the post-reset start fired a second Started"), Events.StartedCount, 2);
	return true;
}

// A missing definition and a dead component both refuse to start; clearing
// the dead flag reopens starts with a fresh instance id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011MissingDefinitionAndDeadRefuseStart,
	"UEMMO.Tasks.M1_011.MissingDefinitionAndDeadRefuseStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011MissingDefinitionAndDeadRefuseStart::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FLifecycleEvents Events;
	Events.Bind(*Component);

	TestFalse(TEXT("TryStartAttack with an unknown id is rejected"),
		Component->TryStartAttack(FName(TEXT("Unknown")), 1));
	const FCombatSnapshot AfterMissing = Component->GetSnapshot();
	TestTrue(TEXT("a rejected unknown id leaves the component Free"),
		AfterMissing.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("a rejected unknown id mints no InstanceId"), AfterMissing.InstanceId, uint64(0));
	TestEqual(TEXT("a rejected unknown id fires no Started"), Events.StartedCount, 0);
	TestFalse(TEXT("a rejected unknown id fires no Finished"), Events.FinishedCount != 0);

	TestTrue(TEXT("a valid id still starts after a rejected unknown id"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TickN(*Component, 1);
	TestFalse(TEXT("an unknown id is also rejected while attacking"),
		Component->TryStartAttack(FName(TEXT("Unknown")), 1));
	TestEqual(TEXT("the rejected mid-attack start keeps the running instance"),
		Component->GetSnapshot().InstanceId, uint64(1));
	Component->ResetCombat();

	Component->SetDead(true);
	TestTrue(TEXT("SetDead(true) marks the component dead"), Component->IsDead());
	TestFalse(TEXT("a dead component refuses to start attacks"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TestTrue(TEXT("a dead component stays Free when a start is refused"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("a refused dead start fires no Started"), Events.StartedCount, 1);
	TestFalse(TEXT("a refused dead start fires no Finished"), Events.FinishedCount != 0);

	Component->SetDead(false);
	TestTrue(TEXT("clearing the dead flag reopens attack starts"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TestEqual(TEXT("the post-death start still mints a fresh InstanceId"),
		Component->GetSnapshot().InstanceId, uint64(2));
	return true;
}

// The Facing passed to TryStartAttack is carried verbatim into the snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011FacingPropagatesToSnapshot,
	"UEMMO.Tasks.M1_011.FacingPropagatesToSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011FacingPropagatesToSnapshot::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	TestTrue(TEXT("precondition: launcher starts facing -1"), Component->TryStartAttack(FName(TEXT("launcher")), -1));
	TestEqual(TEXT("Facing -1 is carried into the snapshot"), Component->GetSnapshot().Facing, -1);

	Component->ResetCombat();
	TestTrue(TEXT("precondition: light_01 starts facing 1"), Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TestEqual(TEXT("Facing 1 is carried into the snapshot"), Component->GetSnapshot().Facing, 1);
	return true;
}

// The frozen flag is passed straight through to the FCombatClock: a frozen
// frame drops its delta, advances no frame and never ends the attack.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_011FrozenClockPausesFrameAdvance,
	"UEMMO.Tasks.M1_011.FrozenClockPausesFrameAdvance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_011FrozenClockPausesFrameAdvance::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	FLifecycleEvents Events;
	Events.Bind(*Component);

	TestTrue(TEXT("precondition: light_01 starts"), Component->TryStartAttack(FName(TEXT("light_01")), 1));

	Component->SetClockFrozen(true);
	TestTrue(TEXT("SetClockFrozen(true) is reported"), Component->IsClockFrozen());
	TickN(*Component, 3);
	const FCombatSnapshot Frozen = Component->GetSnapshot();
	TestEqual(TEXT("frozen ticks advance no frame"), Frozen.Frame, -1);
	TestTrue(TEXT("the attack is still running while frozen"),
		Frozen.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("frozen ticks fire no Finished"), Events.FinishedCount, 0);

	Component->SetClockFrozen(false);
	TickN(*Component, 1);
	TestEqual(TEXT("the first unfrozen tick lands on Frame 0"), Component->GetSnapshot().Frame, 0);
	return true;
}

#endif
