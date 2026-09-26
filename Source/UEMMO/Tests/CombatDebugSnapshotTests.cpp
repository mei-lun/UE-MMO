#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "InputMappingContext.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_028
{
	// Builds the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path M1-010/M1-011 use) and injects it into a fresh combat
	// component. Returns null after reporting the failure so the caller can
	// bail out early.
	static UCombatComponent* M1_028_NewCombatComponentWithCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("InitializeFromConfig succeeds with the explicit DefaultGame.ini references"),
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
	static void M1_028_TickN(UCombatComponent& Component, int32 Ticks)
	{
		for (int32 Index = 0; Index < Ticks; ++Index)
		{
			Component.TickCombat(1.0f / 60.0f);
		}
	}
}

using namespace UE::UEMMO::Tasks::M1_028;

// An idle component's snapshot carries the debug defaults (Free, Frame -1, no
// AttackId, empty buffer) and exposes no definition, so the overlay would draw
// neither a running frame nor a hit box.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028IdleSnapshotCarriesDebugDefaults,
	"UEMMO.Tasks.M1_028.IdleSnapshotCarriesDebugDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028IdleSnapshotCarriesDebugDefaults::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = M1_028_NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	const FCombatSnapshot Idle = Component->GetSnapshot();
	TestTrue(TEXT("an idle component reports ActionState Free"), Idle.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("an idle component reports Frame -1"), Idle.Frame, -1);
	TestEqual(TEXT("an idle component reports InstanceId 0"), Idle.InstanceId, uint64(0));
	TestEqual(TEXT("an idle component reports no AttackId"), Idle.AttackId, FName(NAME_None));
	TestEqual(TEXT("an idle component reports BufferSize 0"), Idle.BufferSize, 0);
	TestTrue(TEXT("an idle component exposes no current definition (the overlay draws no box)"),
		Component->GetCurrentDefinition() == nullptr);
	return true;
}

// A running light_01 instance shows in the snapshot (Attacking, Frame,
// Facing, AttackId, InstanceId, BufferSize) and the debug accessors expose the
// exact definition and feet origin the real hit query shares, so ComputeHitBox
// through the debug path reproduces the M1-017 mirrored numbers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028AttackingSnapshotCarriesRunningInstanceAndDefinition,
	"UEMMO.Tasks.M1_028.AttackingSnapshotCarriesRunningInstanceAndDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028AttackingSnapshotCarriesRunningInstanceAndDefinition::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = M1_028_NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	TestTrue(TEXT("TryStartAttack('light_01', -1) succeeds from Free"),
		Component->TryStartAttack(FName(TEXT("light_01")), -1));

	const FCombatSnapshot Started = Component->GetSnapshot();
	TestTrue(TEXT("snapshot ActionState is Attacking right after the start"),
		Started.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("snapshot AttackId carries light_01"), Started.AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("the first start mints InstanceId 1"), Started.InstanceId, uint64(1));
	TestEqual(TEXT("snapshot Frame stays -1 until the first tick"), Started.Frame, -1);
	TestEqual(TEXT("snapshot Facing carries -1"), Started.Facing, -1);
	TestEqual(TEXT("snapshot BufferSize stays 0"), Started.BufferSize, 0);

	const UAttackDefinition* Definition = Component->GetCurrentDefinition();
	if (!TestNotNull(TEXT("the attacking component exposes the running attack definition"), Definition))
	{
		return true;
	}
	TestEqual(TEXT("the exposed definition is the catalog's light_01"), Definition->AttackId, FName(TEXT("light_01")));
	TestEqual(TEXT("the exposed definition is the real asset (26 frames)"), Definition->DurationFrames, 26);

	// Two ticks land on Frame 1; the definition stays exposed while running.
	M1_028_TickN(*Component, 2);
	TestEqual(TEXT("two ticks advance the snapshot Frame to 1"), Component->GetSnapshot().Frame, 1);
	TestTrue(TEXT("the definition stays exposed while the instance runs"),
		Component->GetCurrentDefinition() != nullptr);

	// World-less component: no owner, so the debug feet origin reads zero and
	// the shared geometry still produces the M1-017 numbers mirrored by the
	// snapshot Facing (-1).
	const FVector Feet = Component->GetDebugFeetLocation();
	TestTrue(TEXT("a world-less component resolves the debug feet origin to zero"), Feet.IsZero());
	const FCombatHitBox Box = ComputeHitBox(Feet, Component->GetSnapshot().Facing, *Definition);
	TestTrue(TEXT("facing -1 mirrors the offset X to -95"), FMath::IsNearlyEqual(Box.Center.X, -95.0, 0.01));
	TestTrue(TEXT("facing -1 keeps the offset Y 0"), FMath::IsNearlyEqual(Box.Center.Y, 0.0, 0.01));
	TestTrue(TEXT("facing -1 keeps the offset Z 90"), FMath::IsNearlyEqual(Box.Center.Z, 90.0, 0.01));
	TestTrue(TEXT("the extent passes through as (85, 50, 70)"),
		FMath::IsNearlyEqual(Box.Extent.X, 85.0, 0.01) &&
		FMath::IsNearlyEqual(Box.Extent.Y, 50.0, 0.01) &&
		FMath::IsNearlyEqual(Box.Extent.Z, 70.0, 0.01));

	// light_01 runs 26 frames total: the instance finishes back to Free and
	// the definition access closes again (no box drawn afterwards).
	M1_028_TickN(*Component, 24);
	TestTrue(TEXT("the finished instance reports ActionState Free"),
		Component->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("no definition is exposed after the instance finished"),
		Component->GetCurrentDefinition() == nullptr);
	return true;
}

// After SetDead the component refuses TryStartAttack and the snapshot stays
// empty (death is tracked separately from the action state), so the overlay
// would show Free with no AttackId and no box.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028DeadComponentSnapshotStaysEmptyAndRefusesAttacks,
	"UEMMO.Tasks.M1_028.DeadComponentSnapshotStaysEmptyAndRefusesAttacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028DeadComponentSnapshotStaysEmptyAndRefusesAttacks::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = M1_028_NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	Component->SetDead(true);
	TestTrue(TEXT("SetDead marks the component dead"), Component->IsDead());
	TestFalse(TEXT("a dead component refuses TryStartAttack"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	TestFalse(TEXT("a dead component refuses movement"), Component->CanAcceptMovement());

	const FCombatSnapshot Dead = Component->GetSnapshot();
	TestTrue(TEXT("the dead snapshot keeps ActionState Free (death is tracked separately)"),
		Dead.ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the dead snapshot carries no AttackId"), Dead.AttackId, FName(NAME_None));
	TestEqual(TEXT("the dead snapshot keeps Frame -1"), Dead.Frame, -1);
	TestEqual(TEXT("the dead snapshot keeps BufferSize 0"), Dead.BufferSize, 0);
	TestTrue(TEXT("a dead component exposes no definition (no box for the overlay)"),
		Component->GetCurrentDefinition() == nullptr);
	return true;
}

// The debug overlay switch defaults to off, the first toggle turns it on, the
// second turns it off again, and the explicit setter drives both directions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028DebugToggleDefaultsOffAndTogglesBothWays,
	"UEMMO.Tasks.M1_028.DebugToggleDefaultsOffAndTogglesBothWays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028DebugToggleDefaultsOffAndTogglesBothWays::RunTest(const FString& Parameters)
{
	APrototypeHUD* HUD = NewObject<APrototypeHUD>();
	TestFalse(TEXT("the debug overlay defaults to off"), HUD->IsCombatDebugOverlayEnabled());
	HUD->ToggleCombatDebugOverlay();
	TestTrue(TEXT("the first toggle turns the overlay on"), HUD->IsCombatDebugOverlayEnabled());
	HUD->ToggleCombatDebugOverlay();
	TestFalse(TEXT("the second toggle turns the overlay off again"), HUD->IsCombatDebugOverlayEnabled());
	HUD->SetCombatDebugOverlayEnabled(true);
	TestTrue(TEXT("SetCombatDebugOverlayEnabled(true) switches on"), HUD->IsCombatDebugOverlayEnabled());
	HUD->SetCombatDebugOverlayEnabled(false);
	TestFalse(TEXT("SetCombatDebugOverlayEnabled(false) switches off"), HUD->IsCombatDebugOverlayEnabled());
	return true;
}

// The debug box path shares the real query exactly: the pinned feet origin
// flows through GetDebugFeetLocation, the catalog's light_01 flows through
// GetCurrentDefinition and ComputeHitBox reproduces the M1-017 right-facing
// numbers; the left-facing mirror is spot-checked on the same function.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028DebugHitBoxPathMatchesM1_017Numbers,
	"UEMMO.Tasks.M1_028.DebugHitBoxPathMatchesM1_017Numbers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028DebugHitBoxPathMatchesM1_017Numbers::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = M1_028_NewCombatComponentWithCatalog(*this);
	if (Component == nullptr)
	{
		return true;
	}

	// Pin the same feet origin M1-017 uses and start the real light_01.
	Component->SetFeetLocationProvider([]()
	{
		return FVector(100.0, 20.0, 0.0);
	});
	TestTrue(TEXT("TryStartAttack('light_01', 1) succeeds"),
		Component->TryStartAttack(FName(TEXT("light_01")), 1));
	const UAttackDefinition* Definition = Component->GetCurrentDefinition();
	TestNotNull(TEXT("the attacking component exposes the running definition"), Definition);
	if (Definition == nullptr)
	{
		return true;
	}

	const FVector Feet = Component->GetDebugFeetLocation();
	TestTrue(TEXT("the debug feet origin is the pinned provider value"),
		FMath::IsNearlyEqual(Feet.X, 100.0, 0.01) &&
		FMath::IsNearlyEqual(Feet.Y, 20.0, 0.01) &&
		FMath::IsNearlyEqual(Feet.Z, 0.0, 0.01));

	const FCombatHitBox DebugBox = ComputeHitBox(Feet, Component->GetSnapshot().Facing, *Definition);
	TestTrue(TEXT("the debug path reproduces the M1-017 right-facing center (195, 20, 90)"),
		FMath::IsNearlyEqual(DebugBox.Center.X, 195.0, 0.01) &&
		FMath::IsNearlyEqual(DebugBox.Center.Y, 20.0, 0.01) &&
		FMath::IsNearlyEqual(DebugBox.Center.Z, 90.0, 0.01));
	TestTrue(TEXT("the debug path reproduces the M1-017 extent (85, 50, 70)"),
		FMath::IsNearlyEqual(DebugBox.Extent.X, 85.0, 0.01) &&
		FMath::IsNearlyEqual(DebugBox.Extent.Y, 50.0, 0.01) &&
		FMath::IsNearlyEqual(DebugBox.Extent.Z, 70.0, 0.01));

	// Same function, left facing: only the offset X mirrors (center 5, 20, 90).
	UAttackDefinition* LocalAttack = NewObject<UAttackDefinition>();
	LocalAttack->HitOffsetFromFeet = FVector(95.0f, 0.0f, 90.0f);
	LocalAttack->HitHalfExtent = FVector(85.0f, 50.0f, 70.0f);
	const FCombatHitBox MirroredBox = ComputeHitBox(FVector(100.0, 20.0, 0.0), -1, *LocalAttack);
	TestTrue(TEXT("left facing mirrors only the offset X (center 5, 20, 90)"),
		FMath::IsNearlyEqual(MirroredBox.Center.X, 5.0, 0.01) &&
		FMath::IsNearlyEqual(MirroredBox.Center.Y, 20.0, 0.01) &&
		FMath::IsNearlyEqual(MirroredBox.Center.Z, 90.0, 0.01));
	return true;
}

// The HUD resolves the first training enemy once into a weak cache and safely
// falls back to "no target" after the enemy is destroyed (no crash, no stale
// reads); the same resolution leaves the player empty in a world without one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028StaleTargetWeakReferenceReadsSafe,
	"UEMMO.Tasks.M1_028.StaleTargetWeakReferenceReadsSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028StaleTargetWeakReferenceReadsSafe::RunTest(const FString& Parameters)
{
	// Remote base so the temp world never collides with arena content
	// (X horizontal, Y depth, Z height; same placement idiom as M1-019).
	const FVector M1_028_SceneBase(120000.0, 120000.0, 600.0);

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_028_TestWorld")));
	if (World == nullptr)
	{
		AddWarning(TEXT("the private temp world could not be created; skipping the stale-target test"));
		return true;
	}

	FActorSpawnParameters Params;
	APrototypeHUD* HUD = World->SpawnActor<APrototypeHUD>(M1_028_SceneBase, FRotator::ZeroRotator, Params);
	TestNotNull(TEXT("a bare HUD spawns in the temp world"), HUD);
	ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(M1_028_SceneBase + FVector(300.0, 0.0, 88.0), FRotator::ZeroRotator, Params);
	TestNotNull(TEXT("the training enemy spawns in the temp world"), Enemy);
	if (HUD == nullptr || Enemy == nullptr)
	{
		return true;
	}

	HUD->RefreshDebugReferences();
	TestTrue(TEXT("the HUD resolves the spawned training enemy as the debug target"),
		HUD->PeekDebugTarget() == Enemy);
	TestTrue(TEXT("a world without a prototype character safely reports no player"),
		HUD->PeekDebugPlayer() == nullptr);

	// Simulate destruction: the weak cache must drop the enemy and the read
	// path must report no target without crashing (the drawn frame would show
	// "Target: none" and rescan on the next frame).
	Enemy->MarkAsGarbage();
	HUD->RefreshDebugReferences();
	TestTrue(TEXT("after the target is destroyed the HUD safely reports no target"),
		HUD->PeekDebugTarget() == nullptr);
	return true;
}

// The F1 debug-toggle mapping exists exactly once on the runtime mapping
// context (no conflict with W/A/S/D/Space/R/J/K), its action carries the
// Started binding, and a repeated input setup neither duplicates the mapping
// nor rebuilds the action.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_028F1BindingExistsOnRuntimeMapping,
	"UEMMO.Tasks.M1_028.F1BindingExistsOnRuntimeMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_028F1BindingExistsOnRuntimeMapping::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = NewObject<APrototypeCharacter>();
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();

	Character->SetupPlayerInputComponent(Input);
	const UInputMappingContext* Mapping = Character->GetRuntimeInputMappingContext();
	TestNotNull(TEXT("the runtime mapping context exists after input setup"), Mapping);
	if (Mapping == nullptr)
	{
		return true;
	}

	const UInputAction* F1Action = nullptr;
	int32 F1MappingCount = 0;
	for (const FEnhancedActionKeyMapping& MappingEntry : Mapping->GetMappings())
	{
		if (MappingEntry.Key == EKeys::F1)
		{
			++F1MappingCount;
			F1Action = MappingEntry.Action;
		}
	}
	TestEqual(TEXT("exactly one F1 mapping exists on the runtime context"), F1MappingCount, 1);
	TestNotNull(TEXT("the F1 mapping targets a runtime action"), F1Action);

	bool bStartedBound = false;
	for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
	{
		if (Binding.IsValid() && Binding->GetTriggerEvent() == ETriggerEvent::Started && Binding->GetAction() == F1Action)
		{
			bStartedBound = true;
		}
	}
	TestTrue(TEXT("the F1 action carries the Started binding (one toggle per press)"), bStartedBound);

	Character->SetupPlayerInputComponent(Input);
	int32 F1MappingCountAfter = 0;
	for (const FEnhancedActionKeyMapping& MappingEntry : Mapping->GetMappings())
	{
		if (MappingEntry.Key == EKeys::F1)
		{
			++F1MappingCountAfter;
		}
	}
	TestEqual(TEXT("a repeated setup does not duplicate the F1 mapping"), F1MappingCountAfter, 1);
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
