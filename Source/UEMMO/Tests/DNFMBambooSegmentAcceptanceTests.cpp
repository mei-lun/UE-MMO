// Dedicated rendered acceptance for the M8-012 bamboo P02 segment: walk band,
// wall blocks, jump and foreground occlusion on L_DNFM_Bamboo01.
// Kept outside UEMMO.Tasks because a real -game map load must run in its own
// process (same rule as the M6_015 visual acceptance).
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMBambooSegmentAcceptance,
    "UEMMO.Acceptance.M8_012.BambooSegment",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// Frozen constants of the map as built by import_dnfm_bamboo_map.py
// (M8-010 calibration, hypothesis A "value 0 walkable", tile variant P):
// walkable band Y in [422.76, 1127.36] cm, X in [0, 1578.30] cm. The walls
// sit just outside; the pawn capsule radius (~42 cm) adds clearance.
namespace BambooSegment
{
    constexpr float BandYMin = 422.76f;
    constexpr float BandYMax = 1127.36f;
    constexpr float BandXMin = 0.0f;
    constexpr float BandXMax = 1578.30f;
    constexpr float WallTolerance = 80.f; // wall face + capsule radius + margin
}

bool FDNFMBambooSegmentAcceptance::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("dedicated rendered game runs the bamboo segment map"),
        World && World->WorldType == EWorldType::Game
        && World->GetMapName().Contains(TEXT("L_DNFM_Bamboo01")) && FApp::CanEverRender())) return false;

    FString Output;
    if (!TestTrue(TEXT("unique evidence directory is supplied"),
        FParse::Value(FCommandLine::Get(), TEXT("DNFMBambooOutput="), Output))) return false;
    IFileManager::Get().MakeDirectory(*Output, true);

    // The raw -game map boots into the flow's Menu state (the Map Select
    // overlay gates gameplay), so the test enters the segment through the
    // SAME production path the menu button uses: a transient room definition
    // whose map is the bamboo segment.
    UGameInstance* Instance = World->GetGameInstance();
    UGameFlowSubsystem* Flow = Instance ? Instance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("game flow subsystem exists"), Flow)) return false;
    URoomDefinition* BambooDef = NewObject<URoomDefinition>();
    BambooDef->RoomId = TEXT("room_bamboo_segment");
    BambooDef->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/UEMMO/DNFM/Bamboo/Maps/L_DNFM_Bamboo01")));
    BambooDef->PlayerSpawnLocation = FVector(789.15f, 775.06f, 100.0f);
    TestTrue(TEXT("bamboo room entry accepted from the menu state"), Flow->EnterRoom(BambooDef));

    const auto Player = []() { return Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)); };
    const auto FeetZ = [](APrototypeCharacter* Pawn)
    {
        return Pawn->GetActorLocation().Z
            - Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    };
    // Drive-until phases follow the M6_015 latent idiom: the command lambda
    // applies the axes and yields (returns false) each frame until the
    // condition accepts or the cap expires - the game thread must keep
    // ticking for the pawn to move.

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
    {
        // Wait for the travel to land in the room state (cap ~15s).
        static double Start = 0;
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (Flow->GetState() == EGameFlowState::Room || FPlatformTime::Seconds() - Start > 15.0)
        {
            Start = 0;
            TestTrue(TEXT("bamboo room travel landed in the room state"),
                Flow->GetState() == EGameFlowState::Room);
            return true;
        }
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, FeetZ, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestNotNull(TEXT("player spawned on the bamboo segment"), Pawn)) return true;
        const FVector Loc = Pawn->GetActorLocation();
        TestTrue(TEXT("spawn stands inside the hypothesis-A walk band"),
            Loc.Y > BambooSegment::BandYMin && Loc.Y < BambooSegment::BandYMax
            && Loc.X > BambooSegment::BandXMin && Loc.X < BambooSegment::BandXMax);
        TestTrue(TEXT("player feet rest on the ground plane (Z=0)"),
            FMath::Abs(FeetZ(Pawn)) < 10.f);
        TestTrue(TEXT("DNFM 2D fighter visual is the active display"),
            Pawn->IsDNFMVisualActive() && !Pawn->GetMesh()->IsVisible());
        FScreenshotRequest::RequestScreenshot(Output / TEXT("feet-ground.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));

    // Walk toward the viewer (+Y) until the near wall vicinity: the wall must
    // stop the pawn inside the band.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Output]()
    {
        static double Start = 0;
        static float OriginY = 0.f;
        APrototypeCharacter* Pawn = Player();
        if (!Pawn) { Start = 0; return true; }
        if (Start == 0) { Start = FPlatformTime::Seconds(); OriginY = Pawn->GetActorLocation().Y; }
        if (Pawn->GetActorLocation().Y >= BambooSegment::BandYMax - 90.f || FPlatformTime::Seconds() - Start > 12.0)
        {
            Pawn->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            const float Y = Pawn->GetActorLocation().Y;
            TestTrue(TEXT("near wall stops the pawn inside the band"),
                Y <= BambooSegment::BandYMax + BambooSegment::WallTolerance);
            TestTrue(TEXT("pawn reached the near wall vicinity"),
                Y >= BambooSegment::BandYMax - 90.f);
            TestTrue(TEXT("pawn actually travelled toward the viewer"), Y - OriginY > 100.f);
            FScreenshotRequest::RequestScreenshot(Output / TEXT("band-near-block.png"), true, false);
            return true;
        }
        Pawn->DevSetMoveAxes(0.f, 1.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));

    // Walk away from the viewer (-Y) until the far wall: the blocked strip
    // must not be enterable.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Output]()
    {
        static double Start = 0;
        APrototypeCharacter* Pawn = Player();
        if (!Pawn) { Start = 0; return true; }
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (Pawn->GetActorLocation().Y <= BambooSegment::BandYMin + 90.f || FPlatformTime::Seconds() - Start > 14.0)
        {
            Pawn->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            const float Y = Pawn->GetActorLocation().Y;
            TestTrue(TEXT("far wall holds; blocked strip is not walkable"),
                Y >= BambooSegment::BandYMin - BambooSegment::WallTolerance);
            TestTrue(TEXT("pawn reached the far wall vicinity"),
                Y <= BambooSegment::BandYMin + 90.f);
            FScreenshotRequest::RequestScreenshot(Output / TEXT("band-far-block.png"), true, false);
            return true;
        }
        Pawn->DevSetMoveAxes(0.f, -1.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));

    // Walk right (+X) toward the occlusion spot, then forward (+Y): the L7
    // foreground fringe (foot line Y~1268 cm, X~1415 cm) must draw over the
    // fighter from this viewpoint.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        static double Start = 0;
        APrototypeCharacter* Pawn = Player();
        if (!Pawn) { Start = 0; return true; }
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (Pawn->GetActorLocation().X >= 1390.f || FPlatformTime::Seconds() - Start > 12.0)
        {
            Pawn->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            const float X = Pawn->GetActorLocation().X;
            TestTrue(TEXT("right walk reached the occlusion column"), X >= 1390.f);
            TestTrue(TEXT("side wall holds; the pawn stayed inside the map"),
                X <= BambooSegment::BandXMax + BambooSegment::WallTolerance);
            return true;
        }
        Pawn->DevSetMoveAxes(1.f, 0.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        static double Start = 0;
        APrototypeCharacter* Pawn = Player();
        if (!Pawn) { Start = 0; return true; }
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (Pawn->GetActorLocation().Y >= 990.f || FPlatformTime::Seconds() - Start > 10.0)
        {
            Pawn->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            TestTrue(TEXT("occlusion approach reached the near band"),
                Pawn->GetActorLocation().Y >= 990.f);
            return true;
        }
        Pawn->DevSetMoveAxes(0.f, 1.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestNotNull(TEXT("player remains for occlusion evidence"), Pawn)) return true;
        const FVector Loc = Pawn->GetActorLocation();
        AddInfo(FString::Printf(TEXT("occlusion pose X=%.1f Y=%.1f Z=%.1f"), Loc.X, Loc.Y, Loc.Z));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("occlusion.png"), true, false);
        return true;
    }));

    // M8-014: walk to the east exit marker (translucent column at X=1560)
    // and capture it; also record mid-map waypoint shots for M8-015.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        static double Start = 0;
        APrototypeCharacter* Pawn = Player();
        if (!Pawn) { Start = 0; return true; }
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (Pawn->GetActorLocation().X >= 1500.f || FPlatformTime::Seconds() - Start > 10.0)
        {
            Pawn->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            TestTrue(TEXT("exit marker reachable: pawn walked to the east gap"),
                Pawn->GetActorLocation().X >= 1500.f);
            return true;
        }
        Pawn->DevSetMoveAxes(1.f, 0.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestNotNull(TEXT("player remains for exit evidence"), Pawn)) return true;
        const FVector Loc = Pawn->GetActorLocation();
        AddInfo(FString::Printf(TEXT("exit pose X=%.1f Y=%.1f"), Loc.X, Loc.Y));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("exit-marker.png"), true, false);
        return true;
    }));

    // Jump: rises, is captured in the air, and lands back on Z=0.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!Pawn) return true;
        Pawn->Jump();
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.45f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, FeetZ, Output]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestNotNull(TEXT("player remains for jump evidence"), Pawn)) return true;
        TestTrue(TEXT("jump leaves the ground"), FeetZ(Pawn) > 40.f);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("jump-air.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, FeetZ]()
    {
        APrototypeCharacter* Pawn = Player();
        if (!TestNotNull(TEXT("player remains for landing evidence"), Pawn)) return true;
        TestTrue(TEXT("landing restores feet to the ground plane"), FMath::Abs(FeetZ(Pawn)) < 10.f);
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    return true;
}
#endif
