// Dedicated rendered acceptance for M8-019B: the goblin thrower's AI throws
// its stone from the hand anchor and the stone lands through the unified
// health chain. Scenarios (the card: hit once + miss once):
//   A  aligned target inside the throw range -> telegraph -> the stone spawns
//      at the hand anchor, flies straight, and the player loses exactly the
//      throw damage through UHealthComponent::ApplyDamage.
//   A2 the same throw after the shader settle (the fresh stone material
//      compiles its permutations async on first use - the M8-019A lesson) ->
//      a live mid-flight screenshot is the rendered evidence.
//   B  depth-misaligned target: the attack-ready condition refuses (pure rule
//      assert) and a room-bounded goblin that can never align never throws.
//   C  aligned at telegraph entry, target steps off-line mid-windup: the
//      stone fires (the position judgment is frozen at entry, the M2-003
//      rule) but flies past - the player's health is unchanged and the stone
//      lands at the range end and removes itself.
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../Enemy/MeleeEnemyController.h"
#include "../Enemy/ThrowStoneProjectile.h"
#include "../Enemy/EnemyDefinition.h"
#include "../PrototypeCharacter.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMGoblinThrowAcceptance,
    "UEMMO.Acceptance.M8_019B.GoblinThrow",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace
{
	int32 CountLiveStones(const UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AThrowStoneProjectile> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}
}

namespace
{
	/**
	 * The production-shaped monster_goblinthrower definition (the enemies.json
	 * row values; the controller only enters the wind-up when the definition
	 * carries a MeleeAttackId - the M2-003 no-definition-never-attacks rule).
	 */
	UEnemyDefinition* MakeGoblinDefinition()
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = FName(TEXT("monster_goblinthrower"));
		Def->MaxHP = 60.0f;
		Def->AttackPower = 6.0f;
		Def->MoveSpeed = 240.0f;
		Def->AttackRangeX = 500.0f;
		Def->AlignYTolerance = 40.0f;
		Def->TelegraphSeconds = 0.3f;
		Def->MeleeAttackId = FName(TEXT("goblin_throw"));
		return Def;
	}
}

bool FDNFMGoblinThrowAcceptance::RunTest(const FString& Parameters)
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

    // Pure-rule evidence first: the attack-ready condition refuses a
    // depth-misaligned target (the card's 距离/纵深不合不打 at the rule level).
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        const FVector Enemy(0.f, 0.f, 100.f);
        TestTrue(TEXT("ready rule: aligned and inside the throw range"),
            ComputeMeleeAttackReady(Enemy, Enemy + FVector(450.f, 0.f, 0.f), 500.f, 40.f));
        TestFalse(TEXT("ready rule: depth-misaligned target never readies"),
            ComputeMeleeAttackReady(Enemy, Enemy + FVector(450.f, 150.f, 0.f), 500.f, 40.f));
        TestFalse(TEXT("ready rule: beyond the throw range never readies"),
            ComputeMeleeAttackReady(Enemy, Enemy + FVector(600.f, 0.f, 0.f), 500.f, 40.f));
        // The approach rule is depth-first (the 纵深走位 alignment step).
        const FVector Intent = ComputeMeleeApproachIntent(Enemy, Enemy + FVector(450.f, 150.f, 0.f), 500.f, 40.f);
        TestTrue(TEXT("approach rule: depth-first alignment intent"),
            FMath::IsNearlyZero(Intent.X) && FMath::Abs(Intent.Y) > 0.9f);
        return true;
    }));

    // Scenario A: aligned target at 450cm (inside the 500 range) - the goblin
    // telegraphs, releases from the hand anchor, and the stone hits.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for scenario A"), P)) return true;
        P->GetHealth()->ResetHealth();
        const FVector PlayerLoc = P->GetActorLocation();
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AGoblinThrowerEnemy* G = GWorld->SpawnActor<AGoblinThrowerEnemy>(
            AGoblinThrowerEnemy::StaticClass(), PlayerLoc + FVector(-450.f, 0.f, 0.f),
            FRotator::ZeroRotator, Params);
        if (!TestNotNull(TEXT("goblin A spawned"), G)) return true;
        G->SpawnDefaultController();
        return true;
    }));
    // Settle first: the capsule must finish its spawn adjustment before the
    // brain wires, so the hand-anchor assert below reads a static goblin.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Goblin]()
    {
        APrototypeCharacter* P = Player();
        AGoblinThrowerEnemy* G = Goblin();
        if (!TestNotNull(TEXT("goblin A for wiring"), G) || !TestNotNull(TEXT("player for wiring"), P)) return true;
        AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(G->GetController());
        if (!TestNotNull(TEXT("goblin A brain"), Brain)) return true;
        G->SetEnemyDefinition(MakeGoblinDefinition());
        Brain->SetTarget(P);
        return true;
    }));
    // Telegraph 0.3s + the release; the stone is mid-flight at ~0.9s.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.9f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Goblin]()
    {
        APrototypeCharacter* P = Player();
        AGoblinThrowerEnemy* G = Goblin();
        if (!TestNotNull(TEXT("goblin A still present"), G) || !TestNotNull(TEXT("player for the flight check"), P)) return true;
        AThrowStoneProjectile* Stone = nullptr;
        for (TActorIterator<AThrowStoneProjectile> It(GWorld); It; ++It) Stone = *It;
        if (!TestNotNull(TEXT("the stone is in flight"), Stone)) return true;
        TestTrue(TEXT("the stone flies toward +X (the target side)"),
            Stone->GetFacing() == 1 && Stone->IsInFlight());
        TestTrue(TEXT("the release point is the hand anchor"),
            FVector::Dist(Stone->GetHandAnchor(),
                G->GetActorLocation()
                    - FVector(0.f, 0.f, G->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight())
                    + FVector(30.f, 0.f, 100.f)) < 2.0f);
        TestTrue(TEXT("the stone keeps the release depth"),
            FMath::Abs(Stone->GetActorLocation().Y - G->GetActorLocation().Y) < 2.0f);
        // Freeze the brain NOW: the stone is an independent actor and keeps
        // flying, while the goblin's throw loop (recover 0.5s -> re-arm) must
        // not fire a second stone into the health assertions below.
        if (AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(G->GetController()))
        {
            Brain->SetTarget(nullptr);
        }
        return true;
    }));
    // The flight to 450cm at 400cm/s takes ~1.1s from the release.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.4f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for the hit check"), P)) return true;
        TestTrue(TEXT("the hit landed exactly the throw damage through the health chain"),
            FMath::IsNearlyEqual(P->GetHealth()->GetHealth(),
                P->GetHealth()->GetMaxHealth() - 6.f, 0.01f));
        TestTrue(TEXT("the stone removed itself after the hit"),
            CountLiveStones(GWorld) == 0);
        return true;
    }));

    // Freeze the brain for the shader settle (the fresh stone material
    // compiles its permutations async on first use - the M8-019A lesson).
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(35.0f));

    // Scenario A2: re-target; the cooldown has long passed, so the goblin
    // throws again and the live stone is captured mid-flight.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player, Goblin]()
    {
        APrototypeCharacter* P = Player();
        AGoblinThrowerEnemy* G = Goblin();
        if (!TestNotNull(TEXT("goblin A2"), G) || !TestNotNull(TEXT("player A2"), P)) return true;
        P->GetHealth()->ResetHealth();
        if (AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(G->GetController()))
        {
            Brain->SetTarget(P);
        }
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.75f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        TestTrue(TEXT("A2: the stone is in flight for the capture"),
            CountLiveStones(GWorld) == 1);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("throw-midflight.png"), true, false);
        // Freeze immediately: the stone flies on independently, the goblin
        // never re-arms, and the health assertion below stays deterministic.
        for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It)
        {
            if (AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>((*It)->GetController()))
            {
                Brain->SetTarget(nullptr);
            }
        }
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for the A2 hit check"), P)) return true;
        TestTrue(TEXT("A2: the second throw landed through the health chain"),
            FMath::IsNearlyEqual(P->GetHealth()->GetHealth(),
                P->GetHealth()->GetMaxHealth() - 6.f, 0.01f));
        return true;
    }));

    // Scenario B: a room-bounded goblin that can never depth-align never
    // throws (the player's depth offset stays beyond the tolerance).
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for scenario B"), P)) return true;
        P->GetHealth()->ResetHealth();
        const FVector PlayerLoc = P->GetActorLocation();
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AGoblinThrowerEnemy* G = GWorld->SpawnActor<AGoblinThrowerEnemy>(
            AGoblinThrowerEnemy::StaticClass(), PlayerLoc + FVector(-450.f, 150.f, 0.f),
            FRotator::ZeroRotator, Params);
        if (!TestNotNull(TEXT("goblin B spawned"), G)) return true;
        G->SpawnDefaultController();
        AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(G->GetController());
        if (!TestNotNull(TEXT("goblin B brain"), Brain)) return true;
        G->SetEnemyDefinition(MakeGoblinDefinition());
        // A zero-size bounds box at the goblin's spot filters every move
        // intent: the depth offset never closes, so the release never runs.
        Brain->SetRoomBounds(FBox(G->GetActorLocation() - FVector(1.f), G->GetActorLocation() + FVector(1.f)));
        Brain->SetTarget(P);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.8f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for the no-throw check"), P)) return true;
        TestTrue(TEXT("B: a depth-misaligned, bounded goblin never throws"),
            CountLiveStones(GWorld) == 0);
        TestTrue(TEXT("B: the player's health is untouched"),
            FMath::IsNearlyEqual(P->GetHealth()->GetHealth(),
                P->GetHealth()->GetMaxHealth(), 0.01f));
        return true;
    }));

    // Scenario C: aligned at telegraph entry, the target steps off-line
    // mid-windup - the stone still fires (the M2-003 frozen judgment) but
    // flies past and lands at the range end without any damage.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for scenario C"), P)) return true;
        P->GetHealth()->ResetHealth();
        const FVector PlayerLoc = P->GetActorLocation();
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
        AGoblinThrowerEnemy* G = GWorld->SpawnActor<AGoblinThrowerEnemy>(
            AGoblinThrowerEnemy::StaticClass(), PlayerLoc + FVector(-450.f, 0.f, 0.f),
            FRotator::ZeroRotator, Params);
        if (!TestNotNull(TEXT("goblin C spawned"), G)) return true;
        G->SpawnDefaultController();
        AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(G->GetController());
        if (!TestNotNull(TEXT("goblin C brain"), Brain)) return true;
        G->SetEnemyDefinition(MakeGoblinDefinition());
        Brain->SetTarget(P);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.15f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        // Mid-windup: the telegraph locked the facing/position - stepping the
        // target off-line must not stop the release (the source rule: a
        // target that walks away gets missed by the real hit query).
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for the mid-windup step"), P)) return true;
        P->SetActorLocation(P->GetActorLocation() + FVector(0.f, 200.f, 0.f));
        return true;
    }));
    // The release lands at ~0.35s; freeze after it so the throw loop stops.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It)
        {
            if (AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>((*It)->GetController()))
            {
                Brain->SetTarget(nullptr);
            }
        }
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.7f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Player]()
    {
        APrototypeCharacter* P = Player();
        if (!TestNotNull(TEXT("player for the miss check"), P)) return true;
        TestTrue(TEXT("C: the off-line stone missed the player"),
            FMath::IsNearlyEqual(P->GetHealth()->GetHealth(),
                P->GetHealth()->GetMaxHealth(), 0.01f));
        TestTrue(TEXT("C: the missed stone landed at the range end and removed itself"),
            CountLiveStones(GWorld) == 0);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
    return true;
}
#endif
