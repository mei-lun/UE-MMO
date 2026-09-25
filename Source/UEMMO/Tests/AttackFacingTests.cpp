#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Character/AttackMovementGate.h"
#include "../Character/PlanarMovement.h"
#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_013
{
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

	// Advances the component by exactly Ticks single 1/60 s frames.
	static void TickN(UCombatComponent& Component, int32 Ticks)
	{
		for (int32 Index = 0; Index < Ticks; ++Index)
		{
			Component.TickCombat(1.0f / 60.0f);
		}
	}
}

using namespace UE::UEMMO::Tasks::M1_013;

// Free and alive: movement and turning are both accepted and the gate keeps
// the full planar speed; a missing combat component is not gated either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_013FreeStateAllowsMovementAndTurn,
	"UEMMO.Tasks.M1_013.FreeStateAllowsMovementAndTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_013FreeStateAllowsMovementAndTurn::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewObject<UCombatComponent>();
	TestTrue(TEXT("a free alive component accepts movement"), Component->CanAcceptMovement());
	TestTrue(TEXT("a free alive component accepts turning"), Component->CanTurn());
	TestTrue(TEXT("the gate keeps the full planar speed while free"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 1.0f));
	TestTrue(TEXT("the gate allows facing flips while free"), CanFlipFacing(Component));

	// No combat subobject: the gate stays open (M0 behavior, defensive default).
	TestTrue(TEXT("a missing combat component leaves the gate open"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(nullptr), 1.0f));
	TestTrue(TEXT("a missing combat component still allows facing flips"), CanFlipFacing(nullptr));
	return true;
}

// Attacking: active X/Y movement and turning are refused for the whole attack,
// the left-facing start carries Facing -1 into the snapshot and attack ticks
// never rotate the locked facing; after a reset a right-facing start carries 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_013AttackingBlocksMovementTurnAndLocksFacing,
	"UEMMO.Tasks.M1_013.AttackingBlocksMovementTurnAndLocksFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_013AttackingBlocksMovementTurnAndLocksFacing::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	// Left-facing attack start (card: snapshot direction for a left attack).
	TestTrue(TEXT("precondition: light_01 starts facing left (-1)"),
		Component->TryStartAttack(FName(TEXT("light_01")), -1));
	const FCombatSnapshot Started = Component->GetSnapshot();
	TestTrue(TEXT("snapshot ActionState is Attacking right after the start"),
		Started.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the left-facing start carries Facing -1 into the snapshot"), Started.Facing, -1);

	TestFalse(TEXT("an attacking component refuses movement"), Component->CanAcceptMovement());
	TestFalse(TEXT("an attacking component refuses turning"), Component->CanTurn());
	TestTrue(TEXT("the gate scales active planar input to zero while attacking"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 0.0f));
	TestFalse(TEXT("the gate locks facing flips while attacking"), CanFlipFacing(Component));

	// Mid-attack: the locked facing survives the ticks and stays refused.
	TickN(*Component, 5);
	TestEqual(TEXT("attack ticks keep the locked Facing at -1"), Component->GetSnapshot().Facing, -1);
	TestFalse(TEXT("turning stays refused mid-attack"), Component->CanTurn());
	TestTrue(TEXT("active movement stays scaled to zero mid-attack"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 0.0f));

	// Right-facing start after a reset (card: snapshot direction for both sides).
	Component->ResetCombat();
	TestTrue(TEXT("precondition: light_01 starts again facing right (1)"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TestEqual(TEXT("the right-facing start carries Facing 1 into the snapshot"),
		Component->GetSnapshot().Facing, 1);
	TestFalse(TEXT("the right-facing attack still refuses turning"), Component->CanTurn());
	return true;
}

// Death has the highest priority (contract section 4): dead refuses movement
// and turning in every state, alive or attacking, and a revived component is
// gated again exactly until the running attack ends.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_013DeadBlocksMovementAndTurnInEveryState,
	"UEMMO.Tasks.M1_013.DeadBlocksMovementAndTurnInEveryState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_013DeadBlocksMovementAndTurnInEveryState::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	// Dead while Free.
	Component->SetDead(true);
	TestTrue(TEXT("SetDead(true) marks the component dead"), Component->IsDead());
	TestFalse(TEXT("a dead free component refuses movement"), Component->CanAcceptMovement());
	TestFalse(TEXT("a dead free component refuses turning"), Component->CanTurn());
	TestTrue(TEXT("the dead gate scales active planar input to zero"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 0.0f));
	TestFalse(TEXT("the dead gate locks facing flips"), CanFlipFacing(Component));
	TestFalse(TEXT("a dead component cannot start an attack"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));

	// Dead while Attacking (started alive, death mid-flight).
	Component->SetDead(false);
	TestTrue(TEXT("precondition: light_01 starts alive"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	Component->SetDead(true);
	TestFalse(TEXT("a dead attacking component refuses movement"), Component->CanAcceptMovement());
	TestFalse(TEXT("a dead attacking component refuses turning"), Component->CanTurn());
	TestTrue(TEXT("death keeps the planar gate closed mid-attack"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 0.0f));

	// Reviving mid-attack: the running attack still gates movement and turn.
	Component->SetDead(false);
	TestTrue(TEXT("reviving mid-attack keeps the attack running"),
		Component->GetSnapshot().ActionState == ECombatActionState::Attacking);
	TestFalse(TEXT("a revived mid-attack component still refuses movement"), Component->CanAcceptMovement());
	TestFalse(TEXT("a revived mid-attack component still refuses turning"), Component->CanTurn());

	// Once the attack ends the gates open again.
	TickN(*Component, 26);
	TestTrue(TEXT("precondition: the attack ended back to Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("movement is accepted again after the attack ends"), Component->CanAcceptMovement());
	TestTrue(TEXT("turning is accepted again after the attack ends"), Component->CanTurn());
	TestTrue(TEXT("the gate returns to full speed after the attack ends"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 1.0f));
	return true;
}

// Both gate-opening paths restore movement and turning immediately: the attack
// reaching its final frame and a mid-attack ResetCombat teardown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_013FinishAndResetRestoreMovementAndTurn,
	"UEMMO.Tasks.M1_013.FinishAndResetRestoreMovementAndTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_013FinishAndResetRestoreMovementAndTurn::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	// Natural end (light_01 covers 26 frames; the 26th step returns to Free).
	TestTrue(TEXT("precondition: light_01 starts"), Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TickN(*Component, 5);
	TestFalse(TEXT("precondition: movement is refused mid-attack"), Component->CanAcceptMovement());
	TickN(*Component, 21);
	TestTrue(TEXT("the attack ended back to Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the natural end restores movement"), Component->CanAcceptMovement());
	TestTrue(TEXT("the natural end restores turning"), Component->CanTurn());
	TestTrue(TEXT("the gate returns to full speed after the natural end"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 1.0f));
	TestTrue(TEXT("facing flips are allowed again after the natural end"), CanFlipFacing(Component));

	// Reset mid-attack restores the gates immediately.
	TestTrue(TEXT("precondition: light_01 starts again"), Component->TryStartAttack(FName(TEXT("light_01")), -1));
	TickN(*Component, 3);
	TestFalse(TEXT("precondition: movement is refused before the reset"), Component->CanAcceptMovement());
	Component->ResetCombat();
	TestTrue(TEXT("ResetCombat restores movement immediately"), Component->CanAcceptMovement());
	TestTrue(TEXT("ResetCombat restores turning immediately"), Component->CanTurn());
	TestTrue(TEXT("the gate returns to full speed after the reset"),
		FMath::IsNearlyEqual(ComputeAllowedMoveScale(Component), 1.0f));
	TestTrue(TEXT("facing flips are allowed again after the reset"), CanFlipFacing(Component));
	return true;
}

// The gate composes with the M1-029 planar speed: Free keeps the original
// per-axis speed, Attacking scales the active input speed to zero, and death
// scales it to zero as well.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_013GateScalesPlanarVelocity,
	"UEMMO.Tasks.M1_013.GateScalesPlanarVelocity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_013GateScalesPlanarVelocity::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}
	using namespace UE::UEMMO::Tasks::M1_029;

	// Free: full original X speed (420 cm/s) survives the gate.
	const FVector FreeVelocity = ComputePlanarVelocity(1.0f, 0.0f) * ComputeAllowedMoveScale(Component);
	TestTrue(TEXT("free gate keeps the original X speed 420"),
		FMath::IsNearlyEqual(FreeVelocity.X, PlanarSpeedX, 0.01f));

	// Attacking: the active input speed is scaled to zero.
	TestTrue(TEXT("precondition: light_01 starts"), Component->TryStartAttack(FName(TEXT("light_01")), 1));
	const FVector AttackingVelocity = ComputePlanarVelocity(1.0f, 0.0f) * ComputeAllowedMoveScale(Component);
	TestTrue(TEXT("attacking gate scales the active input speed to zero"), AttackingVelocity.IsNearlyZero());

	// Dead: the active input speed is scaled to zero as well.
	Component->ResetCombat();
	Component->SetDead(true);
	const FVector DeadVelocity = ComputePlanarVelocity(1.0f, 0.0f) * ComputeAllowedMoveScale(Component);
	TestTrue(TEXT("dead gate scales the active input speed to zero"), DeadVelocity.IsNearlyZero());
	return true;
}

#endif
