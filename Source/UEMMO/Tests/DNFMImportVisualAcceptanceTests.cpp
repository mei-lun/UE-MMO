// M6-022 dedicated rendered acceptance: the imported DNFM picture set plays
// on the real pawn (UEMMO.PlayerDNFMVisual=1) after a real menu travel, and
// the idle/move/attack displays are captured for the evidence trail. Kept
// outside UEMMO.Tasks because a real OpenLevel must run in its own process
// (the FDNFMPlayerVisualAcceptance precedent it follows).
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../UI/MapSelectWidget.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Combat/CombatComponent.h"
#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/SideViewCameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/Button.h"
#include "Containers/UnrealString.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMImportVisualAcceptance,
    "UEMMO.Acceptance.M6_022.DNFMVisual",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FDNFMImportVisualAcceptance::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
    UGameFlowSubsystem* Flow = Instance ? Instance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    if (!TestTrue(TEXT("dedicated rendered game starts in the menu"),
        World && World->WorldType == EWorldType::Game && Flow
        && Flow->GetState() == EGameFlowState::Menu && FApp::CanEverRender())) return false;
    IConsoleVariable* Gate = IConsoleManager::Get().FindConsoleVariable(TEXT("UEMMO.PlayerDNFMVisual"));
    if (!TestNotNull(TEXT("the DNFM gate exists"), Gate)) return false;
    if (!TestTrue(TEXT("launch with -DPCVars=UEMMO.PlayerDNFMVisual=1"), Gate->GetInt() == 1)) return false;

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
    FScreenshotRequest::RequestScreenshot(Output / TEXT("menu.png"), true, false);
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, RoomId]()
    {
        APlayerController* PC = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
        UMapSelectWidget* Menu = HUD ? HUD->PeekMenuWidget() : nullptr;
        if (!TestNotNull(TEXT("real menu widget exists before entry"), Menu)) return true;
        UButton* Button = nullptr;
        for (int32 i = 0; i < Menu->PeekCatalogRowCount(); ++i)
        {
            UMapSelectCatalogRow* Row = Menu->PeekCatalogRow(i);
            if (Row && Row->GetRowDefinition() && Row->GetRowDefinition()->RoomId.ToString() == RoomId)
                Button = Row->PeekRowButton();
        }
        if (TestNotNull(TEXT("destination button exists"), Button)
            && TestTrue(TEXT("destination button is visible and enabled"), Button->IsVisible() && Button->GetIsEnabled()))
            Button->OnClicked.Broadcast();
        TestTrue(TEXT("real menu click accepted entry"), Flow->GetState() == EGameFlowState::Room);
        return true;
    }));

    struct FState { FVector Origin = FVector::ZeroVector; bool Ready = false; };
    const TSharedRef<FState> State = MakeShared<FState>();
    const auto Player = []() { return Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)); };

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, Player, Map, Output, State]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestTrue(TEXT("travel landed in the requested room, outside menu state"),
            GWorld && GWorld->GetMapName().Contains(Map) && Flow->GetState() == EGameFlowState::Room)
            || !TestNotNull(TEXT("player exists after travel"), Pawn)) return true;
        UDNFMCharacterVisualComponent* Visual = Pawn->GetDNFMVisual();
        if (!TestNotNull(TEXT("the DNFM picture component exists"), Visual)) return true;
        // The M6-022 wiring proof in a real game process: the imported set is
        // bound, the rest-frame calibration ran, and the plane displays the
        // full outfit while the 3D hero mesh stays hidden.
        TestNotNull(TEXT("the imported visual set is bound"), Visual->GetVisualSet());
        TestTrue(TEXT("the rest-frame calibration produced a positive cm/pixel"), Visual->GetCmPerPixel() > 0.f);
        TestTrue(TEXT("the picture plane is visible"), Visual->IsPlaneVisible());
        TestTrue(TEXT("the 3D hero mesh is hidden"), !Pawn->GetMesh()->IsVisible());
        TestTrue(TEXT("the idle display plays the rest clip"),
            Visual->GetCurrentActionId() == FName(TEXT("rest")));
        AddInfo(FString::Printf(TEXT("idle action=%s frame=%d cm/px=%f plane=%s"),
            *Visual->GetCurrentActionId().ToString(), Visual->GetCurrentFrameIndex(),
            Visual->GetCmPerPixel(), Visual->IsPlaneVisible() ? TEXT("visible") : TEXT("hidden")));
        if (UStaticMeshComponent* Plane = Visual->GetPlaneComponent())
        {
            AddInfo(FString::Printf(TEXT("plane t=%s vis=%d hidden_ingame=%d castshadow=%d"),
                *Plane->GetComponentTransform().ToString(), Plane->IsVisible() ? 1 : 0,
                Plane->bHiddenInGame ? 1 : 0, Plane->CastShadow ? 1 : 0));
            AddInfo(FString::Printf(TEXT("plane bounds=%s mesh=%s"),
                *Plane->Bounds.GetBox().ToString(), *GetNameSafe(Plane->GetStaticMesh())));
            if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Plane->GetMaterial(0)))
            {
                AddInfo(FString::Printf(TEXT("mid base=%s page_tex=%s uvmin=%s uvsize=%s"),
                    *GetNameSafe(MID->GetBaseMaterial()),
                    *GetNameSafe(MID->K2_GetTextureParameterValue(TEXT("PageTex"))),
                    *MID->K2_GetVectorParameterValue(TEXT("UVRectMin")).ToString(),
                    *MID->K2_GetVectorParameterValue(TEXT("UVRectSize")).ToString()));
            }
            else AddError(TEXT("plane has no dynamic material instance"));
        }
        else AddError(TEXT("no plane component while the picture is active"));
        if (USideViewCameraComponent* Rig = Pawn->FindComponentByClass<USideViewCameraComponent>())
        {
            const FTransform RigT = Rig->GetComponentTransform();
            const FVector RigY = RigT.GetUnitAxis(EAxis::Y);
            const FVector RigZ = RigT.GetUnitAxis(EAxis::Z);
            AddInfo(FString::Printf(TEXT("rig rot=%s yAxis=%s zAxis=%s pawnRot=%s"),
                *RigT.GetRotation().Rotator().ToString(),
                *RigY.ToString(), *RigZ.ToString(), *Pawn->GetActorRotation().ToString()));
            if (Visual->GetVisualSet() && Visual->GetCmPerPixel() > 0.f)
            {
                const FDNFMActionClip* Rest = Visual->GetVisualSet()->FindAction(FName(TEXT("rest")));
                if (Rest && Rest->Frames.IsValidIndex(Visual->GetCurrentFrameIndex()))
                {
                    const FVector Foot = Pawn->GetCapsuleComponent()->GetComponentLocation()
                        - FVector(0, 0, Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
                    const float Facing = (Pawn->GetActorRotation().Yaw > -90.f && Pawn->GetActorRotation().Yaw < 90.f) ? 1.f : -1.f;
                    const FTransform Expected = UE::UEMMO::Tasks::M6_021::ComputeFrameQuadTransform(
                        Foot, RigY, RigZ, Facing, Rest->Frames[Visual->GetCurrentFrameIndex()], Visual->GetCmPerPixel());
                    AddInfo(FString::Printf(TEXT("expected rot=%s loc=%s"),
                        *Expected.GetRotation().Rotator().ToString(), *Expected.GetTranslation().ToString()));
                }
            }
        }
        State->Ready = true;
        State->Origin = Pawn->GetActorLocation();
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-idle.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Player, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        static double MoveStart = 0;
        if (MoveStart == 0) MoveStart = FPlatformTime::Seconds();
        Pawn->DevSetMoveAxes(1.f, 0.f);
        if (FPlatformTime::Seconds() - MoveStart < 0.8) return false;
        Pawn->DevSetMoveAxes(0.f, 0.f);
        MoveStart = 0;
        TestTrue(TEXT("visible player can move horizontally"), Pawn->GetActorLocation().X - State->Origin.X > 100.f);
        if (UDNFMCharacterVisualComponent* Visual = Pawn->GetDNFMVisual())
            TestTrue(TEXT("the picture follows the walk"), Visual->IsPlaneVisible()
                && Visual->GetCurrentActionId() == FName(TEXT("move")));
        State->Origin = Pawn->GetActorLocation();
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-right.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State]()
    {
        // Keep the attack capture free from random AI interrupts (the real
        // wave creation already ran with the room).
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        for (TActorIterator<AMeleeEnemy> It(GWorld); It; ++It)
        {
            It->SetActorTickEnabled(false);
            It->GetCharacterMovement()->StopMovementImmediately();
            It->GetCharacterMovement()->DisableMovement();
            It->SetActorLocation(Pawn->GetActorLocation() + FVector(1500, 500, 0), false, nullptr, ETeleportType::TeleportPhysics);
        }
        Pawn->SubmitCombatInput(ECombatInput::Light);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.15f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Output, State]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        if (!TestTrue(TEXT("the light attack is live in the combat snapshot"),
            Pawn->GetCombat() && Pawn->GetCombat()->GetSnapshot().AttackId == FName(TEXT("light_01")))) return true;
        if (UDNFMCharacterVisualComponent* Visual = Pawn->GetDNFMVisual())
        {
            AddInfo(FString::Printf(TEXT("attack display action=%s frame=%d"),
                *Visual->GetCurrentActionId().ToString(), Visual->GetCurrentFrameIndex()));
            TestTrue(TEXT("the attack display plays the attack1 clip"),
                Visual->GetCurrentActionId() == FName(TEXT("attack1")));
        }
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-attack.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        for (const TCHAR* Name : { TEXT("dnfm-idle.png"), TEXT("dnfm-right.png"), TEXT("dnfm-attack.png") })
            TestTrue(FString::Printf(TEXT("fresh capture exists: %s"), Name),
                IFileManager::Get().FileSize(*(Output / Name)) > 1024);
        return true;
    }));
    return true;
}
#endif
