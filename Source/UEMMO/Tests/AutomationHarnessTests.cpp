#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_002Harness,
	"UEMMO.Tasks.M1_002.Harness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_002Harness::RunTest(const FString& Parameters)
{
	const bool bGameModuleLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("UEMMO"));
	TestTrue(TEXT("UEMMO game module is loaded in the current process"), bGameModuleLoaded);
	if (bGameModuleLoaded)
	{
		AddInfo(TEXT("UEMMO primary game module is loaded; task-filtered automation entry is functional."));
	}
	return true;
}

#endif
