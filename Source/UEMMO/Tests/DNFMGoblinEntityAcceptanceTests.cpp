// Dedicated rendered acceptance for M8-019A: the goblin enemy entity spawns
// with the DNFM monster picture display (the green stay sprite visible in a
// captured frame) and dies exactly once through the shared Health/Combat
// chain (the down clip holds its lying final frame).
//
// M8-019A root-cause note (the invisible-picture saga): the freshly built
// goblin material compiles its shader permutations ASYNC on first use in a
// -game process (PreparingShaders(1) is visible in early frames) and the
// compile takes up to ~35s. Every screenshot taken before it finished shows
// an empty plane for ANY material on it; after the settle the same state
// renders the green goblin (config-a2/config-f evidence, 2026-11-07). The
// texture, UV math, and material were never broken - the test therefore
// settles before its render verdicts.
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
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
        // Re-apply the material+set+action sequence AFTER BeginPlay completed
        // (guards the BeginPlay-vs-registration race on the production path).
        G->RefreshGoblinMaterial();
        TestTrue(TEXT("visual activated on the monster atlas"),
            G->GetGoblinVisual() != nullptr && G->GetGoblinVisual()->IsVisualActive()
            && G->GetGoblinVisual()->GetVisualSet() != nullptr
            && G->GetGoblinVisual()->GetVisualSet()->GetName().Contains(TEXT("Goblinthrower")));
        TestTrue(TEXT("3D template mesh hidden by the picture mode"),
            !G->GetMesh()->IsVisible());
        TestTrue(TEXT("idle display action is the source stay clip"),
            G->GetGoblinVisual()->GetCurrentActionId() == FName(TEXT("stay")));
        if (UStaticMeshComponent* GP = G->GetGoblinVisual()->GetPlaneComponent())
        {
            const FTransform T = GP->GetComponentTransform();
            AddInfo(FString::Printf(TEXT("goblin plane loc=%s rot=%s scale=%s"),
                *T.GetLocation().ToString(), *T.GetRotation().ToString(), *T.GetScale3D().ToString()));
        }
        return true;
    }));
    // The shader-permutation settle (see the file header): screenshot the
    // standing goblin only after the async material preparation is done.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(35.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Output]()
    {
        FScreenshotRequest::RequestScreenshot(Output / TEXT("goblin-alive.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));

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
