// M6-024 dedicated rendered acceptance: the imported DNFM picture set plays
// the FULL state machine on the real pawn (UEMMO.PlayerDNFMVisual=1) after a
// real menu travel - hit / knockdown / getup sequence / death / revive / jump
// plus every skill's hit pose - and captures each for the evidence trail.
// Drives the EXISTING victim-side entries (NotifyHitReceived,
// BeginLandingRecovery, SetDead) and the public TryStartAttack; no new
// gameplay surface. Kept outside UEMMO.Tasks because a real OpenLevel must
// run in its own process (the FDNFMImportVisualAcceptance precedent).
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../UI/MapSelectWidget.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/SideViewCameraComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/Button.h"
#include "Containers/UnrealString.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMStateVisualAcceptance,
    "UEMMO.Acceptance.M6_024.DNFMStates",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FDNFMStateVisualAcceptance::RunTest(const FString& Parameters)
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

    struct FState { bool Ready = false; };
    const TSharedRef<FState> State = MakeShared<FState>();
    const auto Player = []() { return Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)); };
    const auto VisualOf = []() -> UDNFMCharacterVisualComponent*
    {
        APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
        return Pawn ? Pawn->GetDNFMVisual() : nullptr;
    };

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, Player, VisualOf, Map, Output, State]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestTrue(TEXT("travel landed in the requested room, outside menu state"),
            GWorld && GWorld->GetMapName().Contains(Map) && Flow->GetState() == EGameFlowState::Room)
            || !TestNotNull(TEXT("player exists after travel"), Pawn)) return true;
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the DNFM picture component exists"), Visual)) return true;
        TestNotNull(TEXT("the imported visual set is bound"), Visual->GetVisualSet());
        TestTrue(TEXT("the idle display plays the rest clip"),
            Visual->GetCurrentActionId() == FName(TEXT("rest")));
        // Keep the captures free from random AI interrupts (the room's wave
        // already ran): freeze and teleport every enemy far away.
        for (TActorIterator<AMeleeEnemy> It(GWorld); It; ++It)
        {
            It->SetActorTickEnabled(false);
            It->GetCharacterMovement()->StopMovementImmediately();
            It->GetCharacterMovement()->DisableMovement();
            It->SetActorLocation(Pawn->GetActorLocation() + FVector(1500, 500, 0), false, nullptr, ETeleportType::TeleportPhysics);
        }
        State->Ready = true;
        return true;
    }));

    // --- Hit: the victim-side entry puts the pawn into HitStun; the display
    // shows the alternating damage pose (a 1-frame hold, stable to capture).
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, State]()
    {
        if (!State->Ready) return true;
        APrototypeCharacter* Pawn = Player();
        UCombatComponent* Combat = Pawn ? Pawn->GetCombat() : nullptr;
        if (!TestNotNull(TEXT("the pawn carries the combat component"), Combat)) return true;
        FCombatHit Hit;
        Hit.Target = Pawn;
        Hit.Damage = 1.0f;
        Hit.StunSeconds = 0.9f;
        Hit.HitStopSeconds = 0.0f;
        Hit.AttackId = FName(TEXT("m6_024_probe"));
        Combat->NotifyHitReceived(Hit);
        TestEqual(TEXT("the probe hit opened HitStun"),
            Combat->GetSnapshot().ActionState, ECombatActionState::HitStun);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.35f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf, Output]()
    {
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the hit capture finds the visual"), Visual)) return true;
        const FName Action = Visual->GetCurrentActionId();
        TestTrue(TEXT("the hit display shows a damage pose"),
            Action == FName(TEXT("damage1")) || Action == FName(TEXT("damage2")));
        AddInfo(FString::Printf(TEXT("hit display action=%s frame=%d"),
            *Action.ToString(), Visual->GetCurrentFrameIndex()));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-hit.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.9f)); // the 0.9 s stun ends

    // --- Knockdown + getup: the landing recovery entry runs the real
    // Knockdown -> Recovering lifecycle; down is captured during Knockdown,
    // the overturn->roll sequence during/after Recovering.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* Pawn = Player();
        UCombatComponent* Combat = Pawn ? Pawn->GetCombat() : nullptr;
        if (!TestNotNull(TEXT("the down capture finds combat"), Combat)) return true;
        if (!TestTrue(TEXT("the pawn is Free before the down probe"),
            Combat->GetSnapshot().ActionState == ECombatActionState::Free)) return true;
        UWorld* WorldPtr = GWorld;
        TestTrue(TEXT("the landing recovery entry opened Knockdown"),
            Combat->BeginLandingRecovery(WorldPtr ? WorldPtr->GetTimeSeconds() : 0.0));
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f)); // inside Knockdown
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf, Output]()
    {
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the down capture finds the visual"), Visual)) return true;
        TestEqual(TEXT("the knockdown shows the down clip"),
            Visual->GetCurrentActionId(), FName(TEXT("down")));
        AddInfo(FString::Printf(TEXT("down display frame=%d"), Visual->GetCurrentFrameIndex()));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-down.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.35f)); // 0.65 s: Recovering/roll
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf, Output]()
    {
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the getup capture finds the visual"), Visual)) return true;
        const FName Action = Visual->GetCurrentActionId();
        TestTrue(TEXT("the getup sequence shows overturn or the roll clip"),
            Action == FName(TEXT("overturn"))
            || Action == FName(TEXT("ftrecoveryrollback_body"))
            || Action == FName(TEXT("ftrecoveryrollfront_body")));
        AddInfo(FString::Printf(TEXT("getup display action=%s frame=%d"),
            *Action.ToString(), Visual->GetCurrentFrameIndex()));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-getup.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f)); // the sequence + state end

    // --- Death + revive: SetDead drives the same combat death fact the
    // display consumes; die plays once and holds its end frame; the revive
    // resets to rest at frame 0.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* Pawn = Player();
        UCombatComponent* Combat = Pawn ? Pawn->GetCombat() : nullptr;
        if (!TestNotNull(TEXT("the death capture finds combat"), Combat)) return true;
        Combat->SetDead(true);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f)); // die plays out (570 ms)
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf, Output]()
    {
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the death capture finds the visual"), Visual)) return true;
        TestEqual(TEXT("the death shows the die clip"), Visual->GetCurrentActionId(), FName(TEXT("die")));
        TestEqual(TEXT("the death holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-dead.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.2f)); // the async capture lands while still dead
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* Pawn = Player();
        UCombatComponent* Combat = Pawn ? Pawn->GetCombat() : nullptr;
        if (!TestNotNull(TEXT("the revive finds combat"), Combat)) return true;
        Combat->SetDead(false); // the revival (the health lifecycle's reset face)
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f)); // a tick must see the revive edge
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf]()
    {
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the revive capture finds the visual"), Visual)) return true;
        // The action switch IS the reset proof here (the die end frame was
        // left behind); the frame-0 rebuild face is the M6-023 unit suite's
        // deterministic assertion, this loop has already advanced rest.
        TestEqual(TEXT("the revived display is rest again"),
            Visual->GetCurrentActionId(), FName(TEXT("rest")));
        TestTrue(TEXT("the revived display is a live rest frame"),
            Visual->GetCurrentFrameIndex() == 0 || Visual->GetCurrentFrameIndex() == 1);
        return true;
    }));

    // --- Jump: the real character jump drives the movement facts; the takeoff
    // phase walks the jump clip's rising frames.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestNotNull(TEXT("the jump capture finds the pawn"), Pawn)) return true;
        Pawn->Jump();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf, Output]()
    {
        APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
        UDNFMCharacterVisualComponent* Visual = VisualOf();
        if (!TestNotNull(TEXT("the jump capture finds the visual"), Visual) || !Pawn) return true;
        TestTrue(TEXT("the pawn is airborne mid-jump"), Pawn->GetCharacterMovement()->IsFalling());
        TestEqual(TEXT("the airborne display plays the jump clip"),
            Visual->GetCurrentActionId(), FName(TEXT("jump")));
        AddInfo(FString::Printf(TEXT("jump display frame=%d"), Visual->GetCurrentFrameIndex()));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("dnfm-jump.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.2f)); // land + tail + idle

    // --- Skills: start each attack, freeze the combat clock inside the active
    // window (a real hit stop) so the capture lands exactly on the hit pose.
    struct FSkillCapture { const TCHAR* Name; const TCHAR* AttackId; const TCHAR* ClipId; int32 HitPoseFrame; int32 ActiveStart; };
    const TArray<FSkillCapture> Skills = {
        {TEXT("launcher"),      TEXT("launcher"),      TEXT("liftupper"),      3, 12},
        {TEXT("singlekick"),    TEXT("singlekick"),    TEXT("singlekick"),     3, 11},
        {TEXT("damagelowkick"), TEXT("damagelowkick"), TEXT("damagelowkick"),  2, 11},
        {TEXT("throwsand"),     TEXT("throwsand"),     TEXT("throwsand_sp"),   4, 13},
        {TEXT("earthbreak"),    TEXT("earthbreak"),    TEXT("earthbreak"),     3, 11},
        {TEXT("stomp"),         TEXT("stomp"),         TEXT("stomp"),          2, 11}
    };
    for (const FSkillCapture& Skill : Skills)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Skill]()
        {
            APrototypeCharacter* Pawn = Player();
            UCombatComponent* Combat = Pawn ? Pawn->GetCombat() : nullptr;
            if (!TestTrue(FString::Printf(TEXT("%s starts from Free"), Skill.Name),
                Combat && Combat->GetSnapshot().ActionState == ECombatActionState::Free)) return true;
            TestTrue(FString::Printf(TEXT("%s started"), Skill.Name),
                Combat->TryStartAttack(FName(Skill.AttackId), 1));
            return true;
        }));
        // Poll until the combat clock walks into the active window, then pin
        // it there with a real hit stop (the display frame freezes with it).
        const TSharedRef<double> PollStart = MakeShared<double>(0.0);
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Skill, PollStart]()
        {
            APrototypeCharacter* Pawn = Player();
            UCombatComponent* Combat = Pawn ? Pawn->GetCombat() : nullptr;
            if (!Combat) return true;
            if (*PollStart == 0.0) { *PollStart = FPlatformTime::Seconds(); }
            const FCombatSnapshot Snapshot = Combat->GetSnapshot();
            if (Snapshot.ActionState == ECombatActionState::Attacking
                && Snapshot.Frame >= Skill.ActiveStart)
            {
                Combat->RequestHitStop(0.6f);
                TestTrue(FString::Printf(TEXT("%s pinned inside its active window"), Skill.Name),
                    Combat->IsHitStopActive());
                return true;
            }
            if (Snapshot.ActionState != ECombatActionState::Attacking)
            {
                AddError(FString::Printf(TEXT("%s left Attacking before its active window"), Skill.Name));
                return true;
            }
            if (FPlatformTime::Seconds() - *PollStart > 5.0)
            {
                AddError(FString::Printf(TEXT("%s never reached its active window"), Skill.Name));
                return true;
            }
            return false; // keep polling next frame
        }));
        ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f)); // capture during the freeze
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, VisualOf, Output, Skill]()
        {
            UDNFMCharacterVisualComponent* Visual = VisualOf();
            if (!TestNotNull(FString::Printf(TEXT("%s capture finds the visual"), Skill.Name), Visual)) return true;
            TestEqual(FString::Printf(TEXT("%s shows its mapped clip"), Skill.Name),
                Visual->GetCurrentActionId(), FName(Skill.ClipId));
            TestEqual(FString::Printf(TEXT("%s holds its hit pose"), Skill.Name),
                Visual->GetCurrentFrameIndex(), Skill.HitPoseFrame);
            AddInfo(FString::Printf(TEXT("skill %s action=%s frame=%d"),
                Skill.Name, *Visual->GetCurrentActionId().ToString(), Visual->GetCurrentFrameIndex()));
            FScreenshotRequest::RequestScreenshot(
                Output / (FString(TEXT("dnfm-")) + Skill.Name + TEXT(".png")), true, false);
            return true;
        }));
        ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.9f)); // hitstop + attack end
    }

    // --- The evidence trail: every capture exists and is a fresh PNG.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        for (const TCHAR* Name : { TEXT("menu.png"), TEXT("dnfm-hit.png"), TEXT("dnfm-down.png"),
            TEXT("dnfm-getup.png"), TEXT("dnfm-dead.png"), TEXT("dnfm-jump.png"),
            TEXT("dnfm-launcher.png"), TEXT("dnfm-singlekick.png"), TEXT("dnfm-damagelowkick.png"),
            TEXT("dnfm-throwsand.png"), TEXT("dnfm-earthbreak.png"), TEXT("dnfm-stomp.png") })
        {
            TestTrue(FString::Printf(TEXT("fresh capture exists: %s"), Name),
                IFileManager::Get().FileSize(*(Output / Name)) > 1024);
        }
        return true;
    }));
    return true;
}
#endif
