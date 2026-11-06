// Dedicated rendered acceptance for M8-019A: the goblin enemy entity spawns
// with the DNFM monster picture display and dies exactly once through the
// shared Health/Combat chain. Includes the material bisection matrix that
// localizes the invisible-picture root cause (config A/B/C screenshots).
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMGoblinEntityAcceptance,
    "UEMMO.Acceptance.M8_019A.GoblinEntity",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FDNFMGoblinEntityAcceptance::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("rendered game world"),
        World && World->WorldType == EWorldType::Game && FApp::CanEverRender())) return false;

    FString Output;
    if (!TestTrue(TEXT("evidence directory supplied"),
        FParse::Value(FCommandLine::Get(), TEXT("DNFMGoblinOutput="), Output))) return false;
    IFileManager::Get().MakeDirectory(*Output, true);

    const auto Player = []() { return Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)); };
    const auto Goblin = []() -> AGoblinThrowerEnemy*
    {
        for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It) return *It;
        return nullptr;
    };

    // The raw -game map boots into the flow's Menu state; enter the real
    // training room through the production path so gameplay runs.
    UGameInstance* Instance = World->GetGameInstance();
    UGameFlowSubsystem* Flow = Instance ? Instance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("game flow subsystem exists"), Flow)) return false;
    const URoomDefinition* Training = nullptr;
    for (const URoomDefinition* Candidate : Flow->GetSelectableRoomDefinitions())
        if (Candidate && Candidate->RoomId.ToString() == TEXT("room_training_01")) Training = Candidate;
    if (!TestNotNull(TEXT("training room in the real catalog"), Training)) return false;
    TestTrue(TEXT("training room entry accepted"), Flow->EnterRoom(Training));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
    {
        static double Start = 0;
        if (Start == 0) Start = FPlatformTime::Seconds();
        if (!Flow || Flow->GetState() == EGameFlowState::Room || FPlatformTime::Seconds() - Start > 15.0)
        {
            Start = 0;
            TestTrue(TEXT("training room travel landed"), Flow && Flow->GetState() == EGameFlowState::Room);
            return true;
        }
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin, Player, Output]()
    {
        APrototypeCharacter* PlayerChar = Player();
        if (!TestNotNull(TEXT("player exists to anchor the arena"), PlayerChar)) return true;
        // Spawn the goblin beside the player on the SAME floor spot (the
        // arena's front region occludes planes spawned toward the camera).
        const FVector Loc = PlayerChar->GetActorLocation() + FVector(200.f, 0.f, 50.f);
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AGoblinThrowerEnemy* G = GWorld->SpawnActor<AGoblinThrowerEnemy>(
            AGoblinThrowerEnemy::StaticClass(), Loc, FRotator::ZeroRotator, Params);
        if (!TestNotNull(TEXT("goblin spawned"), G)) return true;
        G->SetActorHiddenInGame(false);
        TestTrue(TEXT("visual activated on the monster atlas"),
            G->GetGoblinVisual() != nullptr && G->GetGoblinVisual()->IsVisualActive()
            && G->GetGoblinVisual()->GetVisualSet() != nullptr
            && G->GetGoblinVisual()->GetVisualSet()->GetName().Contains(TEXT("Goblinthrower")));
        TestTrue(TEXT("3D template mesh hidden by the picture mode"),
            !G->GetMesh()->IsVisible());
        TestTrue(TEXT("idle display action is the source stay clip"),
            G->GetGoblinVisual()->GetCurrentActionId() == FName(TEXT("stay")));
        // M8-019A: dump both planes' full transforms (rotation included).
        if (UStaticMeshComponent* GP = G->GetGoblinVisual()->GetPlaneComponent())
        {
            const FTransform T = GP->GetComponentTransform();
            AddInfo(FString::Printf(TEXT("goblin plane loc=%s rot=%s scale=%s"),
                *T.GetLocation().ToString(), *T.GetRotation().ToString(), *T.GetScale3D().ToString()));
        }
        if (PlayerChar && PlayerChar->GetDNFMVisual() && PlayerChar->GetDNFMVisual()->GetPlaneComponent())
        {
            const FTransform T = PlayerChar->GetDNFMVisual()->GetPlaneComponent()->GetComponentTransform();
            AddInfo(FString::Printf(TEXT("player plane loc=%s rot=%s scale=%s"),
                *T.GetLocation().ToString(), *T.GetRotation().ToString(), *T.GetScale3D().ToString()));
        }
        // M8-019A probe with positive control: the SAME capture technique
        // aimed at the goblin plane AND the player plane (which provably
        // renders in the main view).
        UStaticMeshComponent* ProbePlane = G->GetGoblinVisual()->GetPlaneComponent();
        UStaticMeshComponent* PlayerPlane = PlayerChar && PlayerChar->GetDNFMVisual()
            ? PlayerChar->GetDNFMVisual()->GetPlaneComponent() : nullptr;
        if (ProbePlane != nullptr)
        {
            const auto RunCapture = [this](UStaticMeshComponent* Target, const TCHAR* Label)
            {
                FActorSpawnParameters CapParams;
                AActor* Capture = GWorld->SpawnActor<AActor>(AActor::StaticClass(),
                    Target->GetComponentLocation()
                        + FVector(0.f, 1200.f * FMath::Cos(FMath::DegreesToRadians(18.f)),
                                  1200.f * FMath::Sin(FMath::DegreesToRadians(18.f))),
                    FRotator(-18.f, -90.f, 0.f), CapParams);
                if (Capture == nullptr) { AddInfo(FString::Printf(TEXT("%s capture actor null"), Label)); return; }
                USceneCaptureComponent2D* Cap = NewObject<USceneCaptureComponent2D>(Capture);
                Cap->SetupAttachment(Capture->GetRootComponent());
                Cap->RegisterComponent();
                UTextureRenderTarget2D* Tex = NewObject<UTextureRenderTarget2D>(Capture);
                Tex->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
                Tex->InitAutoFormat(320, 240);
                Tex->UpdateResourceImmediate(true);
                Cap->TextureTarget = Tex;
                Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
                Cap->FOVAngle = 60.f;
                Cap->CaptureScene();
                FTextureRenderTargetResource* Rt = Tex->GameThread_GetRenderTargetResource();
                if (Rt == nullptr) { AddInfo(FString::Printf(TEXT("%s capture RT null"), Label)); Capture->Destroy(); return; }
                TArray<FColor> Pixels;
                if (Rt->ReadPixels(Pixels))
                {
                    int32 Olive = 0;
                    int32 NonBg = 0;
                    for (const FColor& P : Pixels)
                    {
                        if (P.R > 60 || P.G > 60 || P.B > 60) ++NonBg;
                        if (P.G > 80 && P.G >= P.R + 10 && P.G > P.B + 15) ++Olive;
                    }
                    AddInfo(FString::Printf(TEXT("capture %s: nonBg=%d olive=%d of %d"),
                        Label, NonBg, Olive, Pixels.Num()));
                    // Export the raw capture for the coordinator to convert
                    // and inspect (FColor = BGRA in memory).
                    const FString Dir = FPaths::ProjectDir()
                        + TEXT("Artifacts/Tasks/M8-019A/capture-probe/");
                    IFileManager::Get().MakeDirectory(*Dir, true);
                    TArray<uint8> Bytes;
                    Bytes.Append(reinterpret_cast<const uint8*>(Pixels.GetData()),
                        Pixels.Num() * sizeof(FColor));
                    FFileHelper::SaveArrayToFile(Bytes, *(Dir + Label + TEXT(".raw")));
                }
                Capture->Destroy();
            };
            // Flush first so the spawn's render state is settled.
            ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.5f));
            ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, RunCapture, ProbePlane]()
            {
                RunCapture(ProbePlane, TEXT("goblin"));
                return true;
            }));
            ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, RunCapture, PlayerPlane]()
            {
                if (PlayerPlane != nullptr) RunCapture(PlayerPlane, TEXT("player"));
                return true;
            }));
        }
        FScreenshotRequest::RequestScreenshot(Output / TEXT("config-a-goblin-mid.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(35.0f));

    // Material bisection matrix. Config A (baseline): the goblin MID with the
    // goblin page - currently renders nothing. Config B: the goblin MID with
    // the FIGHTER page texture - discriminates texture vs material graph.
    // Config C (positive control): the player's working MID.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin, Player, Output]()
    {
        AGoblinThrowerEnemy* G = Goblin();
        APrototypeCharacter* PlayerChar = Player();
        UStaticMeshComponent* Plane = G && G->GetGoblinVisual()
            ? G->GetGoblinVisual()->GetPlaneComponent() : nullptr;
        if (!TestNotNull(TEXT("goblin plane for the bisection"), Plane)) return true;
        UMaterialInstanceDynamic* GoblinMID = Cast<UMaterialInstanceDynamic>(Plane->GetMaterial(0));
        UMaterialInstanceDynamic* PlayerMID = PlayerChar && PlayerChar->GetDNFMVisual()
            && PlayerChar->GetDNFMVisual()->GetPlaneComponent()
            ? Cast<UMaterialInstanceDynamic>(PlayerChar->GetDNFMVisual()->GetPlaneComponent()->GetMaterial(0))
            : nullptr;
        if (!TestNotNull(TEXT("goblin MID exists"), GoblinMID)) return true;
        if (!TestNotNull(TEXT("player MID exists (control)"), PlayerMID)) return true;
        UTexture* FighterPage = nullptr;
        PlayerMID->GetTextureParameterValue(FName(TEXT("PageTex")), FighterPage);
        UTexture* GoblinPage = nullptr;
        GoblinMID->GetTextureParameterValue(FName(TEXT("PageTex")), GoblinPage);
        AddInfo(FString::Printf(TEXT("pages: goblin=%s fighter=%s"),
            GoblinPage ? *GoblinPage->GetName() : TEXT("null"),
            FighterPage ? *FighterPage->GetName() : TEXT("null")));
        // M8-019A: force the goblin page texture to rebuild its platform
        // data (the RenderOffscreen import saved it before async BC7
        // compression finished; the -game process loaded empty mips).
        if (UTexture2D* GoblinPage2D = Cast<UTexture2D>(GoblinPage))
        {
            GoblinPage2D->UpdateResource();
            AddInfo(TEXT("goblin page texture UpdateResource called"));
        }
        // Cross tests X/Y run as SEPARATE top-level latent commands below
        // (nested ADD_LATENT inside a running lambda never executes).
        UStaticMeshComponent* PlayerPlane = PlayerChar->GetDNFMVisual()->GetPlaneComponent();
        FLinearColor UvMin(0.f, 0.f, 0.f, 0.f);
        FLinearColor UvSize(0.f, 0.f, 0.f, 0.f);
        GoblinMID->GetVectorParameterValue(FName(TEXT("UVRectMin")), UvMin);
        GoblinMID->GetVectorParameterValue(FName(TEXT("UVRectSize")), UvSize);
        // THE decisive texture-content check: read the goblin page's mip0
        // pixels at the stay cell (uv 0.639,0 size 0.026,0.043) and count
        // opaque pixels. 0 opaque => the UE import lost the content.
        UTexture2D* GoblinPage2D = Cast<UTexture2D>(GoblinPage);
        if (GoblinPage2D != nullptr && GoblinPage2D->GetPlatformData()
            && GoblinPage2D->GetPlatformData()->Mips.Num() > 0)
        {
            FTexturePlatformData* PlatformData = GoblinPage2D
                ? const_cast<FTexturePlatformData*>(GoblinPage2D->GetPlatformData()) : nullptr;
            FTexture2DMipMap& Mip = PlatformData->Mips[0];
            const int32 TexW = Mip.SizeX;
            const int32 TexH = Mip.SizeY;
            const int32 CellX = static_cast<int32>(0.639f * TexW);
            const int32 CellY = 0;
            const int32 CellW = static_cast<int32>(0.026f * TexW);
            const int32 CellH = static_cast<int32>(0.043f * TexH);
            void* Raw = Mip.BulkData.Lock(LOCK_READ_ONLY);
            int32 Opaque = 0;
            int32 Total = 0;
            if (Raw != nullptr)
            {
                const FColor* Pixels = static_cast<const FColor*>(Raw);
                for (int32 Y = CellY; Y < FMath::Min(CellY + CellH, TexH); ++Y)
                {
                    for (int32 X = CellX; X < FMath::Min(CellX + CellW, TexW); ++X)
                    {
                        const FColor& P = Pixels[Y * TexW + X];
                        ++Total;
                        if (P.A > 10) ++Opaque;
                    }
                }
                Mip.BulkData.Unlock();
            }
            AddInfo(FString::Printf(TEXT("goblin tex %dx%d stay cell opaque=%d of %d"),
                TexW, TexH, Opaque, Total));
        }
        else AddInfo(TEXT("goblin page texture not a readable Texture2D"));
        if (FighterPage != nullptr)
        {
            // Config B: goblin MID, fighter page (the goblin UVRect keeps
            // sampling the goblin's stay cell coordinates on the fighter
            // page - whatever pixels live there, SOMETHING opaque renders
            // when the material graph works).
            GoblinMID->SetTextureParameterValue(FName(TEXT("PageTex")), FighterPage);
            Plane->SetMaterial(0, GoblinMID);
            Plane->MarkRenderStateDirty();
        }
        FScreenshotRequest::RequestScreenshot(Output / TEXT("config-b-goblin-mid-fighter-page.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin, Player, Output]()
    {
        AGoblinThrowerEnemy* G = Goblin();
        APrototypeCharacter* PlayerChar = Player();
        UStaticMeshComponent* Plane = G && G->GetGoblinVisual()
            ? G->GetGoblinVisual()->GetPlaneComponent() : nullptr;
        if (!TestNotNull(TEXT("goblin plane for config C"), Plane)) return true;
        if (PlayerChar != nullptr && PlayerChar->GetDNFMVisual() != nullptr
            && PlayerChar->GetDNFMVisual()->GetPlaneComponent() != nullptr)
        {
            Plane->SetMaterial(0, PlayerChar->GetDNFMVisual()->GetPlaneComponent()->GetMaterial(0));
        }
        FScreenshotRequest::RequestScreenshot(Output / TEXT("config-c-player-mid.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));

    // Cross test X: the GOBLIN MID on the PLAYER's plane — isolates the MID
    // parent from the plane/owner. Restore the player's MID afterwards.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin, Player, Output]()
    {
        AGoblinThrowerEnemy* G = Goblin();
        APrototypeCharacter* PlayerChar = Player();
        UStaticMeshComponent* GoblinPlane = G && G->GetGoblinVisual()
            ? G->GetGoblinVisual()->GetPlaneComponent() : nullptr;
        if (!TestNotNull(TEXT("goblin plane for X"), GoblinPlane)) return true;
        UMaterialInstanceDynamic* GoblinMID = Cast<UMaterialInstanceDynamic>(GoblinPlane->GetMaterial(0));
        if (!TestNotNull(TEXT("goblin MID for X"), GoblinMID)) return true;
        APrototypeCharacter* PlayerP = Player();
        if (PlayerP != nullptr && PlayerP->GetDNFMVisual() != nullptr
            && PlayerP->GetDNFMVisual()->GetPlaneComponent() != nullptr)
        {
            PlayerP->GetDNFMVisual()->GetPlaneComponent()->SetMaterial(0, GoblinMID);
        }
        FScreenshotRequest::RequestScreenshot(Output / TEXT("test-x-player-plane-goblin-mid.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin, Player, Output]()
    {
        // Cross test Y: the player MID with the GOBLIN page + goblin UVRect
        // on the player plane — isolates texture/params from the MID parent.
        APrototypeCharacter* PlayerP = Player();
        if (!TestNotNull(TEXT("player for Y"), PlayerP)) return true;
        UMaterialInstanceDynamic* PlayerMID = PlayerP->GetDNFMVisual()
            && PlayerP->GetDNFMVisual()->GetPlaneComponent()
            ? Cast<UMaterialInstanceDynamic>(PlayerP->GetDNFMVisual()->GetPlaneComponent()->GetMaterial(0))
            : nullptr;
        if (!TestNotNull(TEXT("player MID for Y"), PlayerMID)) return true;
        AGoblinThrowerEnemy* G = Goblin();
        UStaticMeshComponent* GoblinPlane = G && G->GetGoblinVisual()
            ? G->GetGoblinVisual()->GetPlaneComponent() : nullptr;
        if (GoblinPlane != nullptr)
        {
            UMaterialInstanceDynamic* GoblinMID = Cast<UMaterialInstanceDynamic>(GoblinPlane->GetMaterial(0));
            if (GoblinMID != nullptr)
            {
                UTexture* GoblinPage = nullptr;
                GoblinMID->GetTextureParameterValue(FName(TEXT("PageTex")), GoblinPage);
                FLinearColor UvMin(0.f, 0.f, 0.f, 0.f);
                FLinearColor UvSize(0.f, 0.f, 0.f, 0.f);
                GoblinMID->GetVectorParameterValue(FName(TEXT("UVRectMin")), UvMin);
                GoblinMID->GetVectorParameterValue(FName(TEXT("UVRectSize")), UvSize);
                PlayerMID->SetTextureParameterValue(FName(TEXT("PageTex")), GoblinPage);
                PlayerMID->SetVectorParameterValue(FName(TEXT("UVRectMin")), UvMin);
                PlayerMID->SetVectorParameterValue(FName(TEXT("UVRectSize")), UvSize);
            }
        }
        FScreenshotRequest::RequestScreenshot(Output / TEXT("test-y-player-mid-goblin-page.png"), true, false);
        // Test Z: the player MID with the goblin page at FULL page UV — the
        // cleanest in-game texture-content check. Re-derive the goblin page
        // from the goblin plane's own MID (self-contained scope).
        AGoblinThrowerEnemy* Gz = Goblin();
        UStaticMeshComponent* GoblinPlaneZ = Gz && Gz->GetGoblinVisual()
            ? Gz->GetGoblinVisual()->GetPlaneComponent() : nullptr;
        UTexture* GoblinPageZ = nullptr;
        if (GoblinPlaneZ != nullptr)
        {
            UMaterialInstanceDynamic* GoblinMIDZ = Cast<UMaterialInstanceDynamic>(GoblinPlaneZ->GetMaterial(0));
            if (GoblinMIDZ != nullptr)
            {
                GoblinMIDZ->GetTextureParameterValue(FName(TEXT("PageTex")), GoblinPageZ);
            }
        }
        PlayerMID->SetTextureParameterValue(FName(TEXT("PageTex")), GoblinPageZ);
        PlayerMID->SetVectorParameterValue(FName(TEXT("UVRectMin")), FLinearColor(0.f, 0.f, 0.f, 0.f));
        PlayerMID->SetVectorParameterValue(FName(TEXT("UVRectSize")), FLinearColor(1.f, 1.f, 0.f, 0.f));
        PlayerP->GetDNFMVisual()->GetPlaneComponent()->SetMaterial(0, PlayerMID);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("test-z-full-goblin-page.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));

    // Lethal damage: the down clip holds its last frame; death fires once.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin]()
    {
        AGoblinThrowerEnemy* G = Goblin();
        if (!TestNotNull(TEXT("goblin remains for the death step"), G)) return true;
        const float Before = G->GetHealthComponent()->GetHealth();
        TestTrue(TEXT("lethal damage kills through the shared health chain"),
            G->GetHealthComponent()->ApplyDamage(1000.f) == Before
            && G->GetCombatComponent()->IsDead());
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Goblin, Output]()
    {
        AGoblinThrowerEnemy* G = Goblin();
        if (!TestNotNull(TEXT("goblin remains for the death display"), G)) return true;
        TestTrue(TEXT("death display holds the down clip"),
            G->GetGoblinVisual()->GetCurrentActionId() == FName(TEXT("down")));
        FScreenshotRequest::RequestScreenshot(Output / TEXT("goblin-dead.png"), true, false);
        const float After = G->GetHealthComponent()->GetHealth();
        G->GetHealthComponent()->ApplyDamage(1000.f);
        TestTrue(TEXT("dead goblin refuses further damage"),
            G->GetHealthComponent()->GetHealth() == After);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    return true;
}
#endif
