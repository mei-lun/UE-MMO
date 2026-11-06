// Dedicated real-process acceptance of the public startup, separate from unit tests.
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Skills/SkillLoadoutComponent.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/MapSelectWidget.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResourceExperienceAcceptance,
    "UEMMO.Acceptance.M6_026.ResourceExperience",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FResourceExperienceAcceptance::RunTest(const FString& Parameters)
{
    FString Output;
    if (!TestTrue(TEXT("dedicated output provided"), FParse::Value(FCommandLine::Get(), TEXT("ExperienceOutput="), Output))) return false;
    const bool bExpectRestore = FParse::Param(FCommandLine::Get(), TEXT("ExperienceExpectRestore"));
    const auto Player = []() { return Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)); };
    const auto HUD = []() -> APrototypeHUD* {
        APlayerController* PC = UGameplayStatics::GetPlayerController(GWorld, 0);
        return PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
    };
    const auto Flow = []() -> UGameFlowSubsystem* {
        UGameInstance* GI = GWorld ? GWorld->GetGameInstance() : nullptr;
        return GI ? GI->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    };
    struct FState { FVector Origin = FVector::ZeroVector; float GroundZ = 0; int32 MPBefore = 0; bool Ready = false; };
    const auto State = MakeShared<FState>();
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Flow, State, Output, bExpectRestore]() {
        APrototypeCharacter* Pawn = Player();
        UGameFlowSubsystem* GameFlow = Flow();
        if (!TestTrue(TEXT("default launch automatically enters guild"), GameFlow && GameFlow->IsResourceExperience()
            && GameFlow->GetState() == EGameFlowState::Room && GWorld->GetMapName().Contains(TEXT("L_DNFM_Guildagit")))
            || !TestNotNull(TEXT("player exists"), Pawn)) return true;
        TestTrue(TEXT("latest character visible without a CVar override"), Pawn->IsDNFMVisualActive()
            && Pawn->GetDNFMVisual()->IsPlaneVisible() && !Pawn->GetMesh()->IsVisible());
        USkillLoadoutComponent* Skills = Pawn->GetSkillLoadout();
        TestTrue(TEXT("fighter MP available"), Skills && Skills->GetCurrentMP() > 0);
        for (int32 Slot = 2; Slot <= 6; ++Slot)
            TestFalse(TEXT("each hotbar slot is populated"), Skills->GetSlotSkill(Slot).IsNone());
        if (bExpectRestore) TestTrue(TEXT("second process restores the experience save"), GameFlow->WasStartupProfileRestored());
        UProfileSubsystem* Profile = GWorld->GetGameInstance()->GetSubsystem<UProfileSubsystem>();
        FFileHelper::SaveStringToFile(Profile->GetCharacterId().ToString(), *(Output / TEXT("character-id.txt")));
        // Move opponents away during movement/skill screenshot staging. They are
        // still the actual wave actors, and their deaths later drive real settlement.
        for (TActorIterator<AMeleeEnemy> It(GWorld); It; ++It) {
            It->SetActorLocation(It->GetActorLocation() + FVector(10000, 0, 0));
            It->SetActorTickEnabled(false);
            It->GetCharacterMovement()->DisableMovement();
        }
        State->Origin = Pawn->GetActorLocation(); State->GroundZ = State->Origin.Z; State->Ready = true;
        FScreenshotRequest::RequestScreenshot(Output / TEXT("startup.png"), true, false);
        Pawn->DevSetMoveAxes(1, 0);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State, Output]() {
        if (!State->Ready || !Player()) return true;
        TestTrue(TEXT("horizontal movement works in new display"), Player()->GetActorLocation().X - State->Origin.X > 50);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("right.png"), true, false);
        State->Origin = Player()->GetActorLocation(); Player()->DevSetMoveAxes(0, 1); return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State, Output]() {
        if (!State->Ready || !Player()) return true;
        TestTrue(TEXT("positive depth movement works"), Player()->GetActorLocation().Y - State->Origin.Y > 50);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("depth-front.png"), true, false);
        State->Origin = Player()->GetActorLocation(); Player()->DevSetMoveAxes(0, -1); return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.2f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State, Output]() {
        if (!State->Ready || !Player()) return true;
        TestTrue(TEXT("negative depth movement works"), Player()->GetActorLocation().Y - State->Origin.Y < -50);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("depth-back.png"), true, false);
        Player()->DevSetMoveAxes(0, 0); Player()->StartJump(); return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State, Output]() {
        if (!State->Ready || !Player()) return true;
        TestTrue(TEXT("jump gains height"), Player()->GetActorLocation().Z > State->GroundZ + 20);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("jump.png"), true, false);
        Player()->StopJumping(); return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.9f));
    const TCHAR* AttackIds[] = { TEXT("launcher"), TEXT("singlekick"), TEXT("damagelowkick"), TEXT("throwsand"), TEXT("earthbreak"), TEXT("stomp") };
    for (int32 Skill = 0; Skill < 6; ++Skill) {
        const FName AttackId(AttackIds[Skill]);
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State, Skill]() {
            if (!State->Ready || !Player()) return true;
            // Reset only transient combat resources between the six independent checks.
            Player()->ApplyTrainingRoomReset();
            State->MPBefore = Player()->GetSkillLoadout()->GetCurrentMP();
            if (Skill == 0) Player()->SubmitCombatInput(ECombatInput::Launcher);
            else Player()->SubmitSkillSlot(Skill + 1);
            return true;
        }));
        ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.15f));
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State, Output, AttackId]() {
            if (!State->Ready || !Player()) return true;
            TestEqual(TEXT("input path starts the configured skill"), Player()->GetCombat()->GetSnapshot().AttackId, AttackId);
            TestTrue(TEXT("skill paid MP"), Player()->GetSkillLoadout()->GetCurrentMP() < State->MPBefore);
            FScreenshotRequest::RequestScreenshot(Output / (AttackId.ToString() + TEXT(".png")), true, false);
            return true;
        }));
        ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.9f));
    }
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() {
        if (!State->Ready) return true;
        int32 Alive = 0;
        for (TActorIterator<AMeleeEnemy> It(GWorld); It; ++It)
            if (UHealthComponent* Health = It->FindComponentByClass<UHealthComponent>(); Health && Health->IsAlive()) {
                ++Alive; Health->ApplyDamage(10000);
            }
        TestEqual(TEXT("the real guild wave has two enemies"), Alive, 2);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.7f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, HUD, State]() {
        if (!State->Ready || !HUD()) return true;
        TestTrue(TEXT("real wave deaths produce settlement"), HUD()->HasRoomResultScreen());
        HUD()->HandleRewardClaimRequested();
        HUD()->HandleReturnRequested(); return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, HUD, Flow, Output, State]() {
        if (!State->Ready || !HUD()) return true;
        TestTrue(TEXT("return stays on menu without automatic re-entry"), Flow()->GetState() == EGameFlowState::Menu);
        UMapSelectWidget* Menu = HUD()->PeekMenuWidget();
        if (!TestNotNull(TEXT("single-entry menu exists"), Menu)) return true;
        TestEqual(TEXT("old map entries removed from public menu"), Menu->PeekCatalogRowCount(), 0);
        int32 VisibleButtons = 0;
        TFunction<void(UWidget*)> Count = [&](UWidget* Widget) {
            if (!Widget || !Widget->IsVisible()) return;
            if (Cast<UButton>(Widget)) ++VisibleButtons;
            if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
                for (int32 i = 0; i < Panel->GetChildrenCount(); ++i) Count(Panel->GetChildAt(i));
        };
        Count(Menu->GetRootWidget());
        TestEqual(TEXT("exactly one public entry button"), VisibleButtons, 1);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("menu.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([HUD, State]() {
        if (State->Ready && HUD() && HUD()->PeekMenuWidget()) HUD()->PeekMenuWidget()->PeekEnterButton()->OnClicked.Broadcast();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Flow, State]() {
        if (!State->Ready) return true;
        TestTrue(TEXT("single entry returns to latest resources"), Flow() && Flow()->GetState() == EGameFlowState::Room
            && Player() && Player()->IsDNFMVisualActive());
        return true;
    }));
    return true;
}
#endif
