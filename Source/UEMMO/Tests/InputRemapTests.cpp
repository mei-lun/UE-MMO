#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

#include "../PrototypeCharacter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_040
{
	// World-less hermetic pawn (NewObject: no level load, no BeginPlay), the
	// same pattern the M1-012 routing tests use. The default subobjects exist,
	// so SetupPlayerInputComponent builds the full runtime mapping in place.
	static APrototypeCharacter* MakeCharacter()
	{
		return NewObject<APrototypeCharacter>();
	}

	// Number of IMC mappings from Key onto the runtime action named ActionName.
	// The runtime mapping table (M1-040) is the queryable key-to-action data.
	static int32 CountKeyMappings(const UInputMappingContext& Mapping, const FKey& Key, const TCHAR* ActionName)
	{
		int32 Count = 0;
		for (const FEnhancedActionKeyMapping& Entry : Mapping.GetMappings())
		{
			if (Entry.Key == Key && Entry.Action != nullptr && Entry.Action->GetName() == ActionName)
			{
				++Count;
			}
		}
		return Count;
	}

	// First action the IMC binds Key to; null when the key is unmapped.
	static const UInputAction* FindMappedAction(const UInputMappingContext& Mapping, const FKey& Key)
	{
		for (const FEnhancedActionKeyMapping& Entry : Mapping.GetMappings())
		{
			if (Entry.Key == Key && Entry.Action != nullptr)
			{
				return Entry.Action;
			}
		}
		return nullptr;
	}

	// True when the mapping from Key onto Action carries a negate modifier
	// (the sign convention the planar axis state is built from).
	static bool MappingCarriesNegate(const UInputMappingContext& Mapping, const FKey& Key, const UInputAction& Action)
	{
		for (const FEnhancedActionKeyMapping& Entry : Mapping.GetMappings())
		{
			if (Entry.Key == Key && Entry.Action == &Action)
			{
				for (const TObjectPtr<UInputModifier>& Modifier : Entry.Modifiers)
				{
					if (Modifier != nullptr && Modifier->IsA<UInputModifierNegate>())
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	// Started-event bindings of one action on the component.
	static int32 CountStartedBindings(UEnhancedInputComponent& Input, const UInputAction* Action)
	{
		int32 Count = 0;
		for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input.GetActionEventBindings())
		{
			if (Binding.IsValid() && Binding->GetTriggerEvent() == ETriggerEvent::Started && Binding->GetAction() == Action)
			{
				++Count;
			}
		}
		return Count;
	}

	// The DNF skill-slot keys in slot order: slot 1..8 = Q W E R A S D F.
	const FKey M1_040_SkillSlotKeys[8] = { EKeys::Q, EKeys::W, EKeys::E, EKeys::R, EKeys::A, EKeys::S, EKeys::D, EKeys::F };
}

using namespace UE::UEMMO::Tasks::M1_040;

// The runtime mapping table matches the user-specified DNF layout: X/Z combat,
// C jump with the retained Space alias, arrow-key movement, F2 reset, F1 debug
// toggle, and the old J/K/A/D/W/S/R keys no longer feed their former actions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_040CombatAndMovementKeysMatchDnfLayout,
	"UEMMO.Tasks.M1_040.CombatAndMovementKeysMatchDnfLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_040CombatAndMovementKeysMatchDnfLayout::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();
	Character->SetupPlayerInputComponent(Input);
	const UInputMappingContext* Mapping = Character->GetRuntimeInputMappingContext();
	if (!TestNotNull(TEXT("the runtime mapping context exists after input setup"), Mapping))
	{
		return true;
	}

	// Combat intents: X = Light, Z = Launcher; J/K are removed without alias.
	TestEqual(TEXT("X maps to the CombatLight action"), CountKeyMappings(*Mapping, EKeys::X, TEXT("CombatLight")), 1);
	TestEqual(TEXT("Z maps to the CombatLauncher action"), CountKeyMappings(*Mapping, EKeys::Z, TEXT("CombatLauncher")), 1);
	TestTrue(TEXT("J maps to nothing anymore"), FindMappedAction(*Mapping, EKeys::J) == nullptr);
	TestTrue(TEXT("K maps to nothing anymore"), FindMappedAction(*Mapping, EKeys::K) == nullptr);

	// Jump: C primary; the retained Space alias binds the same action object.
	const UInputAction* JumpAction = FindMappedAction(*Mapping, EKeys::C);
	TestTrue(TEXT("C maps to the Jump action"), JumpAction != nullptr && JumpAction->GetName() == TEXT("Jump"));
	if (JumpAction != nullptr)
	{
		TestTrue(TEXT("Space maps to the same Jump action object (retained alias)"),
			FindMappedAction(*Mapping, EKeys::SpaceBar) == JumpAction);
	}

	// Movement X: Right positive, Left negated; A/D no longer feed movement.
	const UInputAction* MoveHorizontal = FindMappedAction(*Mapping, EKeys::Right);
	TestTrue(TEXT("Right maps to the MoveHorizontal action"),
		MoveHorizontal != nullptr && MoveHorizontal->GetName() == TEXT("MoveHorizontal"));
	TestEqual(TEXT("Left maps to the MoveHorizontal action"), CountKeyMappings(*Mapping, EKeys::Left, TEXT("MoveHorizontal")), 1);
	if (MoveHorizontal != nullptr)
	{
		TestTrue(TEXT("the Right mapping is positive (no negate modifier)"),
			!MappingCarriesNegate(*Mapping, EKeys::Right, *MoveHorizontal));
		TestTrue(TEXT("the Left mapping carries the negate modifier"),
			MappingCarriesNegate(*Mapping, EKeys::Left, *MoveHorizontal));
	}
	TestTrue(TEXT("A no longer maps to MoveHorizontal"), CountKeyMappings(*Mapping, EKeys::A, TEXT("MoveHorizontal")) == 0);
	TestTrue(TEXT("D no longer maps to MoveHorizontal"), CountKeyMappings(*Mapping, EKeys::D, TEXT("MoveHorizontal")) == 0);

	// Movement Y depth: Down positive, Up negated; W/S no longer feed movement.
	const UInputAction* MoveDepth = FindMappedAction(*Mapping, EKeys::Down);
	TestTrue(TEXT("Down maps to the MoveDepth action"),
		MoveDepth != nullptr && MoveDepth->GetName() == TEXT("MoveDepth"));
	TestEqual(TEXT("Up maps to the MoveDepth action"), CountKeyMappings(*Mapping, EKeys::Up, TEXT("MoveDepth")), 1);
	if (MoveDepth != nullptr)
	{
		TestTrue(TEXT("the Down mapping is positive (no negate modifier)"),
			!MappingCarriesNegate(*Mapping, EKeys::Down, *MoveDepth));
		TestTrue(TEXT("the Up mapping carries the negate modifier"),
			MappingCarriesNegate(*Mapping, EKeys::Up, *MoveDepth));
	}
	TestTrue(TEXT("W no longer maps to MoveDepth"), CountKeyMappings(*Mapping, EKeys::W, TEXT("MoveDepth")) == 0);
	TestTrue(TEXT("S no longer maps to MoveDepth"), CountKeyMappings(*Mapping, EKeys::S, TEXT("MoveDepth")) == 0);

	// Reset: F2 in, R out (R became skill slot 4).
	TestEqual(TEXT("F2 maps to the Reset action"), CountKeyMappings(*Mapping, EKeys::F2, TEXT("Reset")), 1);
	TestTrue(TEXT("R no longer maps to the Reset action"), CountKeyMappings(*Mapping, EKeys::R, TEXT("Reset")) == 0);

	// F1 keeps its debug-overlay mapping.
	TestEqual(TEXT("F1 still maps to the DebugToggle action"), CountKeyMappings(*Mapping, EKeys::F1, TEXT("DebugToggle")), 1);

	// M5-034: I maps to the inventory toggle (the production open/close entry).
	TestEqual(TEXT("I maps to the InventoryToggle action"), CountKeyMappings(*Mapping, EKeys::I, TEXT("InventoryToggle")), 1);

	// Total guard: 2 (MoveX) + 2 (MoveY) + 2 (Jump) + 1 (Reset) + 1 (Light)
	// + 1 (Launcher) + 1 (F1) + 8 (skill slots) + 1 (T reload, M5-033)
	// + 1 (I inventory toggle, M5-034); no stale key can hide.
	TestEqual(TEXT("the mapping context carries exactly the DNF layout"), Mapping->GetMappings().Num(), 20);
	return true;
}

// The eight skill-slot keys map in slot order (Q W E R A S D F = slot 1..8)
// onto exactly the runtime actions GetSkillSlotActions exposes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_040SkillSlotKeysMapInSlotOrder,
	"UEMMO.Tasks.M1_040.SkillSlotKeysMapInSlotOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_040SkillSlotKeysMapInSlotOrder::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();
	Character->SetupPlayerInputComponent(Input);
	const UInputMappingContext* Mapping = Character->GetRuntimeInputMappingContext();
	if (!TestNotNull(TEXT("the runtime mapping context exists after input setup"), Mapping))
	{
		return true;
	}

	const TArray<TObjectPtr<UInputAction>>& SlotActions = Character->GetSkillSlotActions();
	TestEqual(TEXT("the runtime exposes exactly eight skill-slot actions"), SlotActions.Num(), 8);
	if (SlotActions.Num() != 8)
	{
		return true;
	}

	for (int32 Slot = 1; Slot <= 8; ++Slot)
	{
		const FKey& SlotKey = M1_040_SkillSlotKeys[Slot - 1];
		const FString ActionName = FString::Printf(TEXT("SkillSlot%d"), Slot);
		TestEqual(FString::Printf(TEXT("slot %d maps exactly once from its DNF key"), Slot),
			CountKeyMappings(*Mapping, SlotKey, *ActionName), 1);
		TestTrue(FString::Printf(TEXT("slot %d key maps onto runtime skill-slot action %d"), Slot, Slot),
			FindMappedAction(*Mapping, SlotKey) == SlotActions[Slot - 1]);
	}
	return true;
}

// SubmitSkillSlot records one press per call into the per-slot counter:
// slots count independently, repeats increment, and out-of-range slots are
// ignored without touching any counter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_040SkillSlotIntentsCountIntoPerSlotCounters,
	"UEMMO.Tasks.M1_040.SkillSlotIntentsCountIntoPerSlotCounters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_040SkillSlotIntentsCountIntoPerSlotCounters::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();

	for (int32 Slot = 1; Slot <= 8; ++Slot)
	{
		TestEqual(FString::Printf(TEXT("slot %d starts at zero presses"), Slot),
			Character->GetSkillSlotPressCount(Slot), 0);
	}

	// One press per slot lands in exactly that slot's counter.
	for (int32 Slot = 1; Slot <= 8; ++Slot)
	{
		Character->SubmitSkillSlot(Slot);
	}
	for (int32 Slot = 1; Slot <= 8; ++Slot)
	{
		TestEqual(FString::Printf(TEXT("slot %d counted its own press"), Slot),
			Character->GetSkillSlotPressCount(Slot), 1);
	}

	// Repeats increment; other slots stay untouched.
	Character->SubmitSkillSlot(3);
	Character->SubmitSkillSlot(3);
	TestEqual(TEXT("repeated presses increment the same slot counter"),
		Character->GetSkillSlotPressCount(3), 3);
	TestEqual(TEXT("a repeat on slot 3 leaves slot 4 untouched"),
		Character->GetSkillSlotPressCount(4), 1);

	// Out-of-range slots are ignored (no crash, no counter movement).
	Character->SubmitSkillSlot(0);
	Character->SubmitSkillSlot(9);
	Character->SubmitSkillSlot(-1);
	TestEqual(TEXT("out-of-range submits changed no counter"),
		Character->GetSkillSlotPressCount(1)
			+ Character->GetSkillSlotPressCount(2)
			+ Character->GetSkillSlotPressCount(5)
			+ Character->GetSkillSlotPressCount(8), 4);
	return true;
}

// The eight skill-slot actions each carry exactly one Started binding, a
// repeated input setup neither stacks duplicates nor rebuilds the actions,
// and the counters keep working after the re-setup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_040SkillSlotBindingsStayIdempotentUnderRepeatedSetup,
	"UEMMO.Tasks.M1_040.SkillSlotBindingsStayIdempotentUnderRepeatedSetup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_040SkillSlotBindingsStayIdempotentUnderRepeatedSetup::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();

	Character->SetupPlayerInputComponent(Input);
	const TArray<TObjectPtr<UInputAction>> SlotsBefore = Character->GetSkillSlotActions();
	TestEqual(TEXT("the first setup exposes eight skill-slot actions"), SlotsBefore.Num(), 8);
	if (SlotsBefore.Num() != 8)
	{
		return true;
	}
	for (int32 Index = 0; Index < 8; ++Index)
	{
		TestEqual(FString::Printf(TEXT("skill-slot action %d carries exactly one Started binding"), Index + 1),
			CountStartedBindings(*Input, SlotsBefore[Index]), 1);
	}

	Character->SetupPlayerInputComponent(Input);
	const TArray<TObjectPtr<UInputAction>> SlotsAfter = Character->GetSkillSlotActions();
	TestEqual(TEXT("a repeated setup does not rebuild the skill-slot actions"), SlotsAfter.Num(), 8);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		TestTrue(FString::Printf(TEXT("skill-slot action %d survived the re-setup unchanged"), Index + 1),
			SlotsAfter[Index] == SlotsBefore[Index]);
		TestEqual(FString::Printf(TEXT("skill-slot action %d still carries exactly one Started binding"), Index + 1),
			CountStartedBindings(*Input, SlotsBefore[Index]), 1);
	}

	Character->SubmitSkillSlot(1);
	Character->SubmitSkillSlot(8);
	TestEqual(TEXT("slot 1 counts normally after the double setup"), Character->GetSkillSlotPressCount(1), 1);
	TestEqual(TEXT("slot 8 counts normally after the double setup"), Character->GetSkillSlotPressCount(8), 1);
	TestEqual(TEXT("the double setup pressed nothing by itself"), Character->GetSkillSlotPressCount(4), 0);
	return true;
}

// Reset routing: F2 targets the Reset action (one Started binding onto the
// M1-027 reset entry, whose behavior the M1-027 suite covers), while R now
// feeds skill slot 4 and no longer touches the Reset action.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_040ResetKeyMovesFromRToF2,
	"UEMMO.Tasks.M1_040.ResetKeyMovesFromRToF2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_040ResetKeyMovesFromRToF2::RunTest(const FString& Parameters)
{
	APrototypeCharacter* Character = MakeCharacter();
	UEnhancedInputComponent* Input = NewObject<UEnhancedInputComponent>();
	Character->SetupPlayerInputComponent(Input);
	const UInputMappingContext* Mapping = Character->GetRuntimeInputMappingContext();
	if (!TestNotNull(TEXT("the runtime mapping context exists after input setup"), Mapping))
	{
		return true;
	}

	// F2 owns the reset mapping; the mapped action is the one Started-bound
	// on the component (the binding target is the unchanged M1-027 entry).
	const UInputAction* ResetAction = FindMappedAction(*Mapping, EKeys::F2);
	TestTrue(TEXT("F2 maps to the Reset action"), ResetAction != nullptr && ResetAction->GetName() == TEXT("Reset"));
	TestEqual(TEXT("the Reset action keeps exactly one Started binding"),
		CountStartedBindings(*Input, ResetAction), 1);

	// R moved to skill slot 4 (Q W E R A S D F = slot 1..8): its mapping is a
	// skill-slot action, not Reset, and the R intent route counts into the
	// slot-4 counter.
	const UInputAction* RAction = FindMappedAction(*Mapping, EKeys::R);
	TestTrue(TEXT("R maps to the SkillSlot4 action"),
		RAction != nullptr && RAction->GetName() == TEXT("SkillSlot4"));
	TestTrue(TEXT("R no longer maps onto the Reset action"), RAction != ResetAction);
	Character->SubmitSkillSlot(4);
	TestEqual(TEXT("the R intent route (slot 4) counted the press"), Character->GetSkillSlotPressCount(4), 1);
	return true;
}

#endif
