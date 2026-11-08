// M8-026 diagnostic: what the packaged boot ACTUALLY shows after the menu
// settles - a delayed in-game screenshot (the early ExecCmds shot raced the
// widget's first paint and showed a bare arena).
#include "Misc/AutomationTest.h"
#include "../PrototypeHUD.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_027MenuBootProbe,
    "UEMMO.Acceptance.M8_027.MenuBootProbe",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_027MenuBootProbe::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("rendered world"), World && World->WorldType == EWorldType::Game)) return false;

    FString Output;
    if (!TestTrue(TEXT("evidence directory supplied"),
        FParse::Value(FCommandLine::Get(), TEXT("DNFMGoblinOutput="), Output))) return false;
    IFileManager::Get().MakeDirectory(*Output, true);

    // The menu settles within the first seconds; screenshot at 8s.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(8.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        APrototypeHUD* Hud = Cast<APrototypeHUD>(UGameplayStatics::GetPlayerController(GWorld, 0)->GetHUD());
        if (!TestNotNull(TEXT("prototype HUD exists"), Hud)) return true;
        // The menu overlay mount state (the HUD logs already carry it; the
        // screenshot carries the visual).
        FScreenshotRequest::RequestScreenshot(Output / TEXT("menu-boot-8s.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    return true;
}
#endif
