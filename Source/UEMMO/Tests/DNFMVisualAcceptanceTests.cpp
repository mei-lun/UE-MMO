// Dedicated rendered acceptance: real menu travel, movement and sprite geometry.
// Kept outside UEMMO.Tasks because a real OpenLevel must run in its own process.
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMPlayerVisualAcceptance,
    "UEMMO.Acceptance.M6_015.PlayerVisual",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FDNFMPlayerVisualAcceptance::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
    UGameFlowSubsystem* Flow = Instance ? Instance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    if (!TestTrue(TEXT("dedicated rendered game starts in the menu"),
        World && World->WorldType == EWorldType::Game && Flow
        && Flow->GetState() == EGameFlowState::Menu && FApp::CanEverRender())) return false;

    FString RoomId = TEXT("room_training_01");
    FParse::Value(FCommandLine::Get(), TEXT("DNFMVisualRoom="), RoomId);
    FString Output;
    if (!TestTrue(TEXT("unique evidence directory is supplied"),
        FParse::Value(FCommandLine::Get(), TEXT("DNFMVisualOutput="), Output))) return false;
    IFileManager::Get().MakeDirectory(*Output, true);
    const URoomDefinition* Room = nullptr;
    for (const URoomDefinition* Candidate : Flow->GetSelectableRoomDefinitions())
        if (Candidate && Candidate->RoomId.ToString() == RoomId) Room = Candidate;
    if (!TestNotNull(TEXT("requested room is in the real catalog"), Room)) return false;
    const FString Map = Room->MapPath.ToSoftObjectPath().GetAssetName();
    if (!TestTrue(TEXT("production menu entry accepted"), Flow->EnterRoom(Room))) return false;

    struct FState { FVector Origin; double Start = 0; float FirstFrame = -1; bool Ready = false; };
    const TSharedRef<FState> State = MakeShared<FState>();
    const auto Player = []() { return Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)); };
    const auto Plane = [](APrototypeCharacter* Pawn) -> UStaticMeshComponent*
    {
        if (!Pawn) return nullptr;
        TArray<UStaticMeshComponent*> Components;
        Pawn->GetComponents(Components);
        for (UStaticMeshComponent* Component : Components)
            if (Component->GetFName() == TEXT("Original2DVisual")) return Component;
        return nullptr;
    };

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, State, Player, Plane, Map, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        UStaticMeshComponent* Visual = Plane(Pawn);
        if (!TestTrue(TEXT("travel landed in the requested room, outside menu state"),
            GWorld && GWorld->GetMapName().Contains(Map) && Flow->GetState() == EGameFlowState::Room)
            || !TestNotNull(TEXT("player exists after travel"), Pawn)
            || !TestNotNull(TEXT("original sprite component exists"), Visual)) return true;
        TestTrue(TEXT("sprite is visible and original 3D mesh is hidden"),
            Visual->IsVisible() && !Pawn->GetMesh()->IsVisible());
        TestTrue(TEXT("sprite presents its face toward the side camera"),
            FMath::Abs(FVector::DotProduct(Visual->GetUpVector(), FVector::YAxisVector)) > 0.9f);
        TestTrue(TEXT("sprite width runs along world X"),
            FMath::Abs(FVector::DotProduct(Visual->GetForwardVector(), FVector::XAxisVector)) > 0.9f);
        if (UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Visual->GetMaterial(0)))
        {
            State->FirstFrame = Material->K2_GetScalarParameterValue(TEXT("Frame"));
            TestTrue(TEXT("atlas chooses one whole frame"),
                FMath::IsNearlyEqual(State->FirstFrame, FMath::FloorToFloat(State->FirstFrame)));
        }
        else AddError(TEXT("sprite has no dynamic material"));
        State->Ready = true;
        State->Origin = Pawn->GetActorLocation();
        FScreenshotRequest::RequestScreenshot(Output / TEXT("player-idle.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Player, Plane, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        if (State->Start == 0) State->Start = FPlatformTime::Seconds();
        Pawn->DevSetMoveAxes(1.f, 0.f);
        if (FPlatformTime::Seconds() - State->Start < 0.8) return false;
        Pawn->DevSetMoveAxes(0.f, 0.f);
        TestTrue(TEXT("visible player can move horizontally after menu travel"),
            Pawn->GetActorLocation().X - State->Origin.X > 100.f);
        State->Origin = Pawn->GetActorLocation();
        State->Start = 0;
        if (UStaticMeshComponent* Visual = Plane(Pawn))
            if (UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Visual->GetMaterial(0)))
                TestTrue(TEXT("idle atlas advances over time"),
                    !FMath::IsNearlyEqual(Material->K2_GetScalarParameterValue(TEXT("Frame")), State->FirstFrame));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("player-right.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Player, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        if (State->Start == 0) State->Start = FPlatformTime::Seconds();
        Pawn->DevSetMoveAxes(0.f, 1.f);
        if (FPlatformTime::Seconds() - State->Start < 0.8) return false;
        Pawn->DevSetMoveAxes(0.f, 0.f);
        TestTrue(TEXT("visible player can move in ground depth"),
            Pawn->GetActorLocation().Y - State->Origin.Y > 100.f);
        State->Start = 0;
        State->Origin = Pawn->GetActorLocation();
        FScreenshotRequest::RequestScreenshot(Output / TEXT("player-depth.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Player, Plane, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        if (State->Start == 0) State->Start = FPlatformTime::Seconds();
        Pawn->DevSetMoveAxes(-1.f, 0.f);
        if (FPlatformTime::Seconds() - State->Start < 0.3) return false;
        Pawn->DevSetMoveAxes(0.f, 0.f);
        if (UStaticMeshComponent* Visual = Plane(Pawn))
        {
            TestTrue(TEXT("sprite remains camera facing after turning left"),
                FMath::Abs(FVector::DotProduct(Visual->GetUpVector(), FVector::YAxisVector)) > 0.9f);
            TestTrue(TEXT("sprite width reverses after turning left"),
                FVector::DotProduct(Visual->GetForwardVector(), FVector::XAxisVector) < -0.9f);
        }
        TestTrue(TEXT("player actually moved left"), Pawn->GetActorLocation().X < State->Origin.X - 50.f);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("player-left.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, Output]()
    {
        for (const TCHAR* Name : { TEXT("player-idle.png"), TEXT("player-right.png"), TEXT("player-depth.png"), TEXT("player-left.png") })
            TestTrue(FString::Printf(TEXT("fresh capture exists: %s"), Name), IFileManager::Get().FileSize(*(Output / Name)) > 1024);
        APlayerController* Controller = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = Controller ? Cast<APrototypeHUD>(Controller->GetHUD()) : nullptr;
        // This training room has no result screen to arm its return-button
        // guard. Exercise the flow and presentation together, without claiming
        // that an unarmed result-screen button was clicked.
        TestTrue(TEXT("flow accepts the room return"), Flow->ReturnToMenu());
        if (TestNotNull(TEXT("production HUD exists for returning"), HUD)) HUD->RefreshMenuPresentation();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
    {
        TestTrue(TEXT("return reaches menu state"), Flow->GetState() == EGameFlowState::Menu);
        APlayerController* Controller = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = Controller ? Cast<APrototypeHUD>(Controller->GetHUD()) : nullptr;
        TestTrue(TEXT("return re-presents the actual menu"), HUD && HUD->HasMenuScreen());
        return true;
    }));
    return true;
}
#endif
