// Dedicated rendered acceptance for M8-019C: the bamboo room's production
// wave loop. The player enters room_bamboo_01 through the real flow entry
// (the same path the menu uses), the map's RoomTrigger fires the run through
// its own overlap (production path, never a test StartRoom call), the wave
// births exactly the two monster_goblinthrower entries of Data/rooms.json
// (the definition's actor class - the goblin thrower itself), the kills go
// through the shared health chain and count once each, and the wave
// settlement opens the room (MarkCleared).
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/RoomTrigger.h"
#include "Components/BoxComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMBambooWaveAcceptance,
    "UEMMO.Acceptance.M8_019C.BambooWave",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace
{
	int32 CountGoblins(const UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AGoblinThrowerEnemy> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}
}

bool FDNFMBambooWaveAcceptance::RunTest(const FString& Parameters)
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
    if (!TestNotNull(TEXT("game flow subsystem exists"), Flow)) return false;
    const URoomDefinition* Bamboo = nullptr;
    for (const URoomDefinition* Candidate : Flow->GetSelectableRoomDefinitions())
        if (Candidate && Candidate->RoomId.ToString() == TEXT("room_bamboo_01")) Bamboo = Candidate;
    if (!TestNotNull(TEXT("bamboo room in the real catalog"), Bamboo)) return false;
    TestTrue(TEXT("bamboo room entry accepted"), Flow->EnterRoom(Bamboo));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
    {
        static double Start = 0;
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (!Flow || Flow->GetState() == EGameFlowState::Room || FPlatformTime::Seconds() - Start > 15.0)
        {
            Start = 0;
            TestTrue(TEXT("bamboo room travel landed"), Flow && Flow->GetState() == EGameFlowState::Room);
            return true;
        }
        return false;
    }));
    // Diagnostics: where the pawn and the trigger actually sit at entry.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        if (APrototypeCharacter* P = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)))
        {
            AddInfo(FString::Printf(TEXT("player at %s"), *P->GetActorLocation().ToString()));
        }
        for (TActorIterator<ARoomTrigger> It(GWorld); It; ++It)
        {
            if (UBoxComponent* Box = (*It)->FindComponentByClass<UBoxComponent>())
            {
                AddInfo(FString::Printf(TEXT("trigger at %s box %s"),
                    *(*It)->GetActorLocation().ToString(),
                    *Box->Bounds.GetBox().ToString()));
            }
        }
        return true;
    }));
    // The production entry: WALK east into the trigger's activation volume
    // (the drive-until pattern; the wave starts on the overlap).
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        static double Start = 0;
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        APrototypeCharacter* P = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
        if (Start == 0) Start = FPlatformTime::Seconds();
        const bool bRunning = Session && Session->GetState() == ERoomSessionState::Running;
        const bool bTimedOut = FPlatformTime::Seconds() - Start > 4.0;
        if (!P || bRunning || bTimedOut)
        {
            if (P) P->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            if (!bRunning) AddInfo(TEXT("the walk-in did not start the run within 4s"));
            return true;
        }
        P->DevSetMoveAxes(1.f, 0.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.5f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        if (!TestNotNull(TEXT("room session subsystem"), Session)) return true;
        TestTrue(TEXT("the trigger overlap started the run (production path)"),
            Session->GetState() == ERoomSessionState::Running);
        // Both rooms.json births (0.3s apart) are the goblin thrower itself.
        TestTrue(TEXT("the wave birthed exactly the two goblin throwers"),
            CountGoblins(GWorld) == 2);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("bamboo-wave-active.png"), true, false);
        return true;
    }));

    // Kill both through the shared health chain; each death counts once.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        int32 Killed = 0;
        for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It)
        {
            AGoblinThrowerEnemy* G = *It;
            if (G != nullptr && G->GetHealthComponent() != nullptr && G->GetHealthComponent()->IsAlive())
            {
                G->GetHealthComponent()->ApplyDamage(1000.f);
                ++Killed;
            }
        }
        TestTrue(TEXT("both wave enemies accepted the lethal damage"), Killed == 2);
        return true;
    }));
    // The wave settlement (the session's 1s wait) closes the room.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        if (!TestNotNull(TEXT("room session subsystem for the clear check"), Session)) return true;
        TestTrue(TEXT("the wave cleared and opened the room (MarkCleared)"),
            Session->GetState() == ERoomSessionState::Cleared);
        TestTrue(TEXT("each death counted exactly once"),
            Session->GetKilledCount() == 2);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("bamboo-wave-cleared.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
    return true;
}
#endif
