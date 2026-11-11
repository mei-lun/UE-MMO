// M8-029: the volcano map's production wave loop (the same acceptance as
// M8-020 but on the volcano map - enter room_volcano_01, trigger, goblin
// wave, combat, settlement).
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_029VolcanoSlice,
    "UEMMO.Acceptance.M8_029.VolcanoSlice",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_029VolcanoSlice::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("rendered game world"),
        World && World->WorldType == EWorldType::Game && FApp::CanEverRender())) return false;

    FString Output;
    if (!TestTrue(TEXT("evidence directory supplied"),
        FParse::Value(FCommandLine::Get(), TEXT("DNFMGoblinOutput="), Output))) return false;
    IFileManager::Get().MakeDirectory(*Output, true);

    UGameInstance* Instance = World->GetGameInstance();
    UGameFlowSubsystem* Flow = Instance ? Instance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("game flow subsystem"), Flow)) return false;
    const URoomDefinition* Volcano = nullptr;
    for (const URoomDefinition* C : Flow->GetSelectableRoomDefinitions())
        if (C && C->RoomId.ToString() == TEXT("room_volcano_01")) Volcano = C;
    if (!TestNotNull(TEXT("volcano room in the catalog"), Volcano)) return false;
    TestTrue(TEXT("volcano room entry accepted"), Flow->EnterRoom(Volcano));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
    {
        static double Start = 0;
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (!Flow || Flow->GetState() == EGameFlowState::Room || FPlatformTime::Seconds() - Start > 15.0)
        {
            Start = 0;
            TestTrue(TEXT("volcano room travel landed"), Flow && Flow->GetState() == EGameFlowState::Room);
            return true;
        }
        return false;
    }));
    // M8-040: walk into the trigger (the production path - the same walk the
    // bamboo slice uses). The M8-029 draft wrongly assumed the spawn already
    // overlaps the trigger and just waited; its recorded evidence was a
    // path-mangled run that never executed the test, so the broken assumption
    // shipped unnoticed.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        static double Start = 0;
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        APrototypeCharacter* P = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
        if (Start == 0) Start = FPlatformTime::Seconds();
        const bool bRunning = Session && Session->GetState() == ERoomSessionState::Running;
        if (!P || bRunning || FPlatformTime::Seconds() - Start > 8.0)
        {
            if (P) P->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            TestTrue(TEXT("the trigger overlap started the run"), bRunning);
            return true;
        }
        P->DevSetMoveAxes(1.f, 0.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        if (!TestNotNull(TEXT("session subsystem"), Session)) return true;
        int32 Goblins = 0;
        for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It) ++Goblins;
        TestTrue(TEXT("the wave birthed the two goblin throwers"), Goblins == 2);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("volcano-wave-active.png"), true, false);
        return true;
    }));

    // Kill both goblins through the shared health chain.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It)
        {
            AGoblinThrowerEnemy* G = *It;
            if (G && G->GetHealthComponent() && G->GetHealthComponent()->IsAlive())
            {
                G->GetHealthComponent()->ApplyDamage(1000.f);
            }
        }
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        if (!TestNotNull(TEXT("session for the clear check"), Session)) return true;
        TestTrue(TEXT("the volcano wave cleared (MarkCleared)"),
            Session->GetState() == ERoomSessionState::Cleared);
        TestTrue(TEXT("both deaths counted exactly once"),
            Session->GetKilledCount() == 2);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("volcano-cleared.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
    return true;
}
#endif
