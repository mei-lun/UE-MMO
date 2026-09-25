#include "Misc/AutomationTest.h"

#include "../Combat/CombatWindow.h"

#if WITH_DEV_AUTOMATION_TESTS

// Acceptance examples for the window [7, 11).
namespace UE::UEMMO::Tasks::M1_005
{
	FCombatWindow MakeWindow(int32 StartFrame, int32 EndFrame)
	{
		FCombatWindow Window;
		Window.StartFrame = StartFrame;
		Window.EndFrame = EndFrame;
		return Window;
	}
}

using namespace UE::UEMMO::Tasks::M1_005;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_005ContainsBoundaries,
	"UEMMO.Tasks.M1_005.ContainsBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_005ContainsBoundaries::RunTest(const FString& Parameters)
{
	const FCombatWindow Window = MakeWindow(7, 11);

	// Half-open interval: start frame is inside, end frame is outside.
	TestTrue(TEXT("[7,11) contains start frame 7"), Window.Contains(7));
	TestTrue(TEXT("[7,11) contains last covered frame 10"), Window.Contains(10));
	TestFalse(TEXT("[7,11) does not contain end frame 11"), Window.Contains(11));
	TestFalse(TEXT("[7,11) does not contain frame 6"), Window.Contains(6));
	TestFalse(TEXT("[7,11) does not contain frame 12"), Window.Contains(12));

	// Degenerate empty window contains nothing even before validation is considered.
	const FCombatWindow Empty = MakeWindow(5, 5);
	TestFalse(TEXT("empty window [5,5) contains nothing"), Empty.Contains(5));

	// Frame 0 belongs to the window [0,1).
	const FCombatWindow FirstFrame = MakeWindow(0, 1);
	TestTrue(TEXT("[0,1) contains frame 0"), FirstFrame.Contains(0));
	TestFalse(TEXT("[0,1) does not contain frame 1"), FirstFrame.Contains(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_005CrossesBoundaries,
	"UEMMO.Tasks.M1_005.CrossesBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_005CrossesBoundaries::RunTest(const FString& Parameters)
{
	const FCombatWindow Window = MakeWindow(7, 11);

	// Acceptance samples for [7,11).
	TestTrue(TEXT("Crosses(6,7) enters window at frame 7"), Window.Crosses(6, 7));
	TestTrue(TEXT("Crosses(6,12) still triggers across the whole window"), Window.Crosses(6, 12));
	TestFalse(TEXT("Crosses(10,11) only passes the end frame which is outside"), Window.Crosses(10, 11));
	TestFalse(TEXT("Crosses(7,7) same frame must not re-trigger"), Window.Crosses(7, 7));

	// Non-forward progress never triggers.
	TestFalse(TEXT("Crosses(12,10) regressed progress returns false"), Window.Crosses(12, 10));
	TestFalse(TEXT("Crosses(11,11) same frame outside window returns false"), Window.Crosses(11, 11));

	// Advancing up to frame 6 has not reached the window yet.
	TestFalse(TEXT("Crosses(-1,6) stops before the window"), Window.Crosses(-1, 6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_005CrossesInitialFrame,
	"UEMMO.Tasks.M1_005.CrossesInitialFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_005CrossesInitialFrame::RunTest(const FString& Parameters)
{
	// With the initial previous frame -1, frame 0 must already be triggerable.
	const FCombatWindow FirstFrame = MakeWindow(0, 1);
	TestTrue(TEXT("[0,1) triggers on Crosses(-1,0)"), FirstFrame.Crosses(-1, 0));
	TestFalse(TEXT("[0,1) does not re-trigger on Crosses(0,0)"), FirstFrame.Crosses(0, 0));

	// A one-frame window later in the timeline still triggers exactly once.
	const FCombatWindow Single = MakeWindow(3, 4);
	TestTrue(TEXT("[3,4) triggers on Crosses(2,3)"), Single.Crosses(2, 3));
	TestFalse(TEXT("[3,4) does not trigger on Crosses(3,4)"), Single.Crosses(3, 4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_005IsValidDuration,
	"UEMMO.Tasks.M1_005.IsValidDuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_005IsValidDuration::RunTest(const FString& Parameters)
{
	// A window that ends exactly at Duration is valid.
	TestTrue(TEXT("[7,11) is valid for Duration 11"), MakeWindow(7, 11).IsValid(11));
	TestTrue(TEXT("[7,11) is valid for a longer Duration"), MakeWindow(7, 11).IsValid(30));
	TestTrue(TEXT("[0,1) is valid for Duration 1"), MakeWindow(0, 1).IsValid(1));

	// Invalid windows.
	TestFalse(TEXT("negative start frame is invalid"), MakeWindow(-1, 3).IsValid(10));
	TestFalse(TEXT("[5,5) empty window is invalid"), MakeWindow(5, 5).IsValid(10));
	TestFalse(TEXT("[6,4) reversed window is invalid"), MakeWindow(6, 4).IsValid(10));
	TestFalse(TEXT("[7,11) exceeds Duration 10"), MakeWindow(7, 11).IsValid(10));
	TestFalse(TEXT("[0,1) exceeds Duration 0"), MakeWindow(0, 1).IsValid(0));
	return true;
}

#endif
