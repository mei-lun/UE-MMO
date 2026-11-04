// Dedicated rendered acceptance: real menu travel, movement and sprite geometry.
// Kept outside UEMMO.Tasks because a real OpenLevel must run in its own process.
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../UI/MapSelectWidget.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/CombatComponent.h"
#include "../Character/PrototypeAnimInstance.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Engine/SkeletalMesh.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/StaticMeshComponent.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
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
    const FVector Spawn = Room->PlayerSpawnLocation;
    const bool bGuild = RoomId == TEXT("room_guildagit_01");
    FScreenshotRequest::RequestScreenshot(Output / TEXT("menu.png"), true, false);
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, RoomId, bGuild]()
    {
        APlayerController* PC = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
        UMapSelectWidget* Menu = HUD ? HUD->PeekMenuWidget() : nullptr;
        if (!TestNotNull(TEXT("real menu widget exists before entry"), Menu)) return true;
        TestTrue(TEXT("entry menu is visible in the viewport"), Menu->IsInViewport() && Menu->IsVisible());
        UButton* Button = bGuild ? Menu->PeekEnterButton() : nullptr;
        if (bGuild)
            TestTrue(TEXT("primary entry names the imported scene"), Menu->PeekEnterLabel()
                && Menu->PeekEnterLabel()->GetText().ToString() == TEXT("Play GuildagitScene"));
        else
            for (int32 i=0; i<Menu->PeekCatalogRowCount(); ++i)
            {
                UMapSelectCatalogRow* Row = Menu->PeekCatalogRow(i);
                if (Row && Row->GetRowDefinition() && Row->GetRowDefinition()->RoomId.ToString()==RoomId)
                    Button=Row->PeekRowButton();
            }
        if (TestNotNull(TEXT("actual destination button exists"), Button)
            && TestTrue(TEXT("destination button is visible and enabled"),Button->IsVisible() && Button->GetIsEnabled()))
            Button->OnClicked.Broadcast();
        TestTrue(TEXT("real menu click accepted entry"), Flow->GetState()==EGameFlowState::Room);
        return true;
    }));

    struct FState { FVector Origin; double Start = 0; float FirstFrame = -1; bool Ready = false; TArray<FTransform> Pose; };
    const TSharedRef<FState> State = MakeShared<FState>();
    const bool bExpect2D=FParse::Param(FCommandLine::Get(),TEXT("DNFMExpect2D"));
    const auto PoseLog=[this](APrototypeCharacter* Pawn,const TCHAR* Stage)
    {
        USkeletalMeshComponent* Mesh=Pawn->GetMesh();
        AddInfo(FString::Printf(TEXT("%s actor=%s mesh=%s bounds=%s"),Stage,*Pawn->GetActorLocation().ToString(),
            *Mesh->GetRelativeTransform().ToString(),*Mesh->Bounds.GetBox().ToString()));
        for(const TCHAR* Bone : {TEXT("root"),TEXT("pelvis"),TEXT("head"),TEXT("foot_l"),TEXT("foot_r")})
            AddInfo(FString::Printf(TEXT("%s %s=%s"),Stage,Bone,*Mesh->GetSocketLocation(FName(Bone)).ToString()));
    };
    const auto PoseChanged=[](USkeletalMeshComponent* Mesh,const TArray<FTransform>& Before)
    {
        const auto& After=Mesh->GetComponentSpaceTransforms();
        if(After.Num()!=Before.Num() || After.Num()<5) return false;
        for(int32 i=1;i<After.Num();++i)
            if(FVector::Dist(After[i].GetLocation(),Before[i].GetLocation())>1.0
                || After[i].GetRotation().AngularDistance(Before[i].GetRotation())>0.05) return true;
        return false;
    };
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
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, State, Player, Plane, Map, Output, Spawn, bGuild, bExpect2D, PoseLog]()
    {
        APrototypeCharacter* Pawn = Player();
        UStaticMeshComponent* Visual = Plane(Pawn);
        if (!TestTrue(TEXT("travel landed in the requested room, outside menu state"),
            GWorld && GWorld->GetMapName().Contains(Map) && Flow->GetState() == EGameFlowState::Room)
            || !TestNotNull(TEXT("player exists after travel"), Pawn)
            || !TestNotNull(TEXT("original sprite component exists"), Visual)) return true;
        APlayerController* PC = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
        TestTrue(TEXT("menu overlay is dismissed after entry"), HUD && !HUD->HasMenuScreen());
        if (bGuild)
        {
            TestTrue(TEXT("imported scene uses its configured player spawn"),
                FVector::Dist2D(Pawn->GetActorLocation(), Spawn)<10.f);
            int32 SceneryCount=0;
            for (TActorIterator<AStaticMeshActor> It(GWorld); It; ++It) ++SceneryCount;
            TestTrue(TEXT("the imported map contains its actual scenery instances"), SceneryCount>=1000);
        }
        if(bExpect2D)
        {
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
        }
        else
        {
            USkeletalMeshComponent* Mesh=Pawn->GetMesh();
            TestTrue(TEXT("animated 3D player is visible; idle sprite is hidden"),Mesh->IsVisible() && !Visual->IsVisible());
            TestTrue(TEXT("actual DNFArt skeletal hero is mounted"), Mesh->GetSkeletalMeshAsset()
                && Mesh->GetSkeletalMeshAsset()->GetName()==TEXT("hero_skm"));
            TestTrue(TEXT("hero has bone geometry and an animation blueprint"),Mesh->GetNumBones()>10
                && Mesh->GetAnimInstance() && Mesh->GetAnimationMode()==EAnimationMode::AnimationBlueprint);
            State->Pose=Mesh->GetComponentSpaceTransforms();
            PoseLog(Pawn,TEXT("idle"));
        }
        State->Ready = true;
        State->Origin = Pawn->GetActorLocation();
        FScreenshotRequest::RequestScreenshot(Output / TEXT("player-idle.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Player, Plane, Output, bExpect2D, PoseChanged]()
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
        if(bExpect2D)
        {
        if (UStaticMeshComponent* Visual = Plane(Pawn))
            if (UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Visual->GetMaterial(0)))
                TestTrue(TEXT("idle atlas advances over time"),
                    !FMath::IsNearlyEqual(Material->K2_GetScalarParameterValue(TEXT("Frame")), State->FirstFrame));
        }
        else
        {
            const UPrototypeAnimInstance* Anim=Cast<UPrototypeAnimInstance>(Pawn->GetMesh()->GetAnimInstance());
            TestTrue(TEXT("run selects moving locomotion state"),Anim && Anim->Speed>100.f && Anim->IsMoving
                && !Anim->IsFalling && Anim->PoseIndex==UPrototypeAnimInstance::Pose_Locomotion);
            const auto& Pose=Pawn->GetMesh()->GetComponentSpaceTransforms();
            bool LegChanged=false;
            for(const TCHAR* Bone:{TEXT("thigh_l"),TEXT("thigh_r"),TEXT("calf_l"),TEXT("calf_r")})
            {
                const int32 Index=Pawn->GetMesh()->GetBoneIndex(Bone);
                if(Pose.IsValidIndex(Index) && State->Pose.IsValidIndex(Index))
                    LegChanged|=Pose[Index].GetRotation().AngularDistance(State->Pose[Index].GetRotation())>0.15;
            }
            TestTrue(TEXT("run changes leg rotations beyond idle breathing"),LegChanged);
        }
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
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Player, Plane, Output, bExpect2D]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!State->Ready || !Pawn) return true;
        if (State->Start == 0) State->Start = FPlatformTime::Seconds();
        Pawn->DevSetMoveAxes(-1.f, 0.f);
        if (FPlatformTime::Seconds() - State->Start < 0.3) return false;
        Pawn->DevSetMoveAxes(0.f, 0.f);
        if (UStaticMeshComponent* Visual = Plane(Pawn); bExpect2D && Visual)
        {
            TestTrue(TEXT("sprite remains camera facing after turning left"),
                FMath::Abs(FVector::DotProduct(Visual->GetUpVector(), FVector::YAxisVector)) > 0.9f);
            TestTrue(TEXT("sprite width reverses after turning left"),
                FVector::DotProduct(Visual->GetForwardVector(), FVector::XAxisVector) < -0.9f);
        }
        TestTrue(TEXT("player actually moved left"), Pawn->GetActorLocation().X < State->Origin.X - 50.f);
        TestTrue(TEXT("3D actor facing follows the leftward input"),Pawn->GetActorForwardVector().X < -0.9f);
        State->Start=0;
        FScreenshotRequest::RequestScreenshot(Output / TEXT("player-left.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this,State,Player,bExpect2D]()
    {
        if(bExpect2D || !State->Ready) return true;
        APrototypeCharacter* Pawn=Player();
        if(!TestNotNull(TEXT("player remains for jump evidence"),Pawn)) return true;
        // Keep the isolated animation captures free from random AI interrupts.
        // Real wave creation was already exercised; death accounting follows.
        // Hit response is driven explicitly below and asserted separately.
        for(TActorIterator<AMeleeEnemy> It(GWorld);It;++It)
        {
            It->SetActorTickEnabled(false);
            It->GetCharacterMovement()->StopMovementImmediately();
            It->GetCharacterMovement()->DisableMovement();
            It->SetActorLocation(Pawn->GetActorLocation()+FVector(1500,500,0),false,nullptr,ETeleportType::TeleportPhysics);
        }
        State->Origin=Pawn->GetActorLocation();
        State->Pose=Pawn->GetMesh()->GetComponentSpaceTransforms();
        Pawn->Jump();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this,State,Player,bExpect2D,Output,PoseChanged,PoseLog]()
    {
        if(bExpect2D || !State->Ready) return true;
        APrototypeCharacter* Pawn=Player();
        if(!Pawn) {AddError(TEXT("jump lost player"));return true;}
        TestTrue(TEXT("3D player jumps above the ground"),Pawn->GetActorLocation().Z>State->Origin.Z+30.f);
        TestTrue(TEXT("jump changes the skeletal pose"),PoseChanged(Pawn->GetMesh(),State->Pose));
        const UPrototypeAnimInstance* Anim=Cast<UPrototypeAnimInstance>(Pawn->GetMesh()->GetAnimInstance());
        TestTrue(TEXT("jump animation state is actually selected"),Anim && Anim->IsFalling
            && Anim->PoseIndex==UPrototypeAnimInstance::Pose_Jump);
        const int32 Calf=Pawn->GetMesh()->GetBoneIndex(TEXT("calf_l"));
        const auto& JumpPose=Pawn->GetMesh()->GetComponentSpaceTransforms();
        TestTrue(TEXT("jump bends the knee beyond idle breathing"),JumpPose.IsValidIndex(Calf)
            && State->Pose.IsValidIndex(Calf)
            && JumpPose[Calf].GetRotation().AngularDistance(State->Pose[Calf].GetRotation())>0.1);
        FScreenshotRequest::RequestScreenshot(Output/TEXT("player-jump.png"),true,false);
        PoseLog(Pawn,TEXT("jump"));
        Pawn->StopJumping();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this,State,Player,bExpect2D]()
    {
        if(bExpect2D || !State->Ready) return true;
        APrototypeCharacter* Pawn=Player();
        if(!Pawn) {AddError(TEXT("attack lost player"));return true;}
        State->Pose=Pawn->GetMesh()->GetComponentSpaceTransforms();
        Pawn->SubmitCombatInput(ECombatInput::Light);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.18f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this,State,Player,bExpect2D,Output,PoseChanged,PoseLog]()
    {
        if(bExpect2D || !State->Ready) return true;
        APrototypeCharacter* Pawn=Player();
        if(!Pawn) {AddError(TEXT("attack pose lost player"));return true;}
        UAnimInstance* Anim=Pawn->GetMesh()->GetAnimInstance();
        AddInfo(FString::Printf(TEXT("attack active=%s combat=%s state=%d"),
            *GetNameSafe(Anim ? Anim->GetCurrentActiveMontage() : nullptr),
            *Pawn->GetCombat()->GetSnapshot().AttackId.ToString(),int32(Pawn->GetCombat()->GetSnapshot().ActionState)));
        UAnimMontage* Active=Anim ? Anim->GetCurrentActiveMontage() : nullptr;
        TestTrue(TEXT("light attack, not hit reaction, is active"),Active && Active->GetName()==TEXT("MNT_light_01")
            && Pawn->GetCombat()->GetSnapshot().AttackId==FName(TEXT("light_01")));
        TestTrue(TEXT("attack changes actual bone transforms"),PoseChanged(Pawn->GetMesh(),State->Pose));
        TestTrue(TEXT("attack keeps body above the floor"),Pawn->GetMesh()->GetSocketLocation(TEXT("head")).Z>Pawn->GetActorLocation().Z);
        FScreenshotRequest::RequestScreenshot(Output/TEXT("player-attack.png"),true,false);
        PoseLog(Pawn,TEXT("attack"));
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this,State,Player,bExpect2D]()
    {
        if(bExpect2D || !State->Ready) return true;
        APrototypeCharacter* Pawn=Player();
        if(!Pawn) {AddError(TEXT("reaction lost player"));return true;}
        State->Pose=Pawn->GetMesh()->GetComponentSpaceTransforms();
        FCombatHit Hit;Hit.Target=Pawn;Hit.StunSeconds=0.5f;Hit.Damage=1.f;
        Pawn->GetCombat()->NotifyHitReceived(Hit);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.06f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this,State,Player,bExpect2D,Output,PoseChanged,PoseLog]()
    {
        if(bExpect2D || !State->Ready) return true;
        APrototypeCharacter* Pawn=Player();
        if(!Pawn) {AddError(TEXT("reaction pose lost player"));return true;}
        UAnimInstance* Anim=Pawn->GetMesh()->GetAnimInstance();
        UAnimMontage* Active=Anim ? Anim->GetCurrentActiveMontage() : nullptr;
        TestTrue(TEXT("hit uses the hero skeleton reaction"),Active && Active->GetName()==TEXT("RCT_Hero_Hit")
            && Active->GetSkeleton()==Pawn->GetMesh()->GetSkeletalMeshAsset()->GetSkeleton());
        TestTrue(TEXT("hit reaction moves the bones without sinking underground"),PoseChanged(Pawn->GetMesh(),State->Pose)
            && Pawn->GetMesh()->GetSocketLocation(TEXT("head")).Z>Pawn->GetActorLocation().Z);
        FScreenshotRequest::RequestScreenshot(Output/TEXT("player-hit.png"),true,false);
        PoseLog(Pawn,TEXT("hit"));
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, Output, bGuild]()
    {
        for (const TCHAR* Name : { TEXT("player-idle.png"), TEXT("player-right.png"), TEXT("player-depth.png"), TEXT("player-left.png") })
            TestTrue(FString::Printf(TEXT("fresh capture exists: %s"), Name), IFileManager::Get().FileSize(*(Output / Name)) > 1024);
        APlayerController* Controller = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = Controller ? Cast<APrototypeHUD>(Controller->GetHUD()) : nullptr;
        if (bGuild)
        {
            // Staged damage, not simulated keyboard combat: let actual death
            // events finish the scene's wave and arm the production result UI.
            URoomSessionSubsystem* Session=GWorld->GetSubsystem<URoomSessionSubsystem>();
            APrototypeCharacter* Pawn=Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld,0));
            if (!TestTrue(TEXT("the guild run is active before clearing"), Session
                && Session->GetState()==ERoomSessionState::Running
                && Session->GetRoomId()==FName(TEXT("room_guildagit_01")))
                || !TestTrue(TEXT("the player survived the scene traversal"),Pawn && Pawn->GetHealth() && Pawn->GetHealth()->IsAlive())) return true;
            int32 Damaged=0;
            for (TActorIterator<AMeleeEnemy> It(GWorld); It; ++It)
                if (UHealthComponent* Health=It->FindComponentByClass<UHealthComponent>())
                    if (Health->IsAlive() && Health->ApplyDamage(10000.f)>0.f) ++Damaged;
            TestTrue(TEXT("the scene spawned its configured enemies"), Damaged==2);
        }
        else
        {
            TestTrue(TEXT("flow accepts the training-room return"), Flow->ReturnToMenu());
            if (TestNotNull(TEXT("production HUD exists for returning"), HUD)) HUD->RefreshMenuPresentation();
        }
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, bGuild]()
    {
        if (!bGuild) return true;
        APlayerController* PC=UGameplayStatics::GetPlayerController(GWorld,0);
        APrototypeHUD* HUD=PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
        URoomSessionSubsystem* Session=GWorld->GetSubsystem<URoomSessionSubsystem>();
        TestTrue(TEXT("both real enemy deaths clear the correct room"), Session
            && Session->GetState()==ERoomSessionState::Cleared
            && Session->GetLastResult().bCleared
            && Session->GetLastResult().RoomId==FName(TEXT("room_guildagit_01"))
            && Session->GetKilledCount()==2);
        TestTrue(TEXT("cleared guild scene presents its return-capable result UI"),HUD && HUD->HasRoomResultScreen());
        if (HUD) HUD->HandleReturnRequested();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, Output]()
    {
        TestTrue(TEXT("return reaches menu state"), Flow->GetState() == EGameFlowState::Menu);
        APlayerController* Controller = UGameplayStatics::GetPlayerController(GWorld, 0);
        APrototypeHUD* HUD = Controller ? Cast<APrototypeHUD>(Controller->GetHUD()) : nullptr;
        UMapSelectWidget* Menu=HUD ? HUD->PeekMenuWidget() : nullptr;
        TestTrue(TEXT("return re-presents the actual menu in the viewport"),Menu && Menu->IsInViewport() && Menu->IsVisible());
        FScreenshotRequest::RequestScreenshot(Output / TEXT("menu-return.png"),true,false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    return true;
}
#endif
