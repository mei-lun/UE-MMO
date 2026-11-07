// Dedicated rendered acceptance for M8-020: the fighter's own attack chain
// kills the bamboo wave goblins - the PLAYER as the damage source (M8-019B
// proved only the goblin->player direction). The production entry chain:
// the real flow catalog entry, the map's RoomTrigger overlap, the wave
// births, then the player closes depth-first and drives light_01 through
// the shared combat pipeline until the wave settles into the Victory state.
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDNFMBambooSliceCombat,
    "UEMMO.Acceptance.M8_020.BambooSliceCombat",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace
{
	AGoblinThrowerEnemy* FindNearestAliveGoblin(const UWorld* World, const FVector& From, float& OutDeltaX, float& OutDeltaY)
	{
		AGoblinThrowerEnemy* Best = nullptr;
		float BestDist = TNumericLimits<float>::Max();
		for (TActorIterator<AGoblinThrowerEnemy> It(World); It; ++It)
		{
			AGoblinThrowerEnemy* G = *It;
			if (G == nullptr || G->GetHealthComponent() == nullptr || !G->GetHealthComponent()->IsAlive())
			{
				continue;
			}
			const FVector Delta = G->GetActorLocation() - From;
			const float Dist = FVector::Dist2D(Delta, FVector::ZeroVector);
			if (Dist < BestDist)
			{
				BestDist = Dist;
				Best = G;
				OutDeltaX = static_cast<float>(Delta.X);
				OutDeltaY = static_cast<float>(Delta.Y);
			}
		}
		return Best;
	}
}

bool FDNFMBambooSliceCombat::RunTest(const FString& Parameters)
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
    if (!TestNotNull(TEXT("bamboo room in the real menu catalog"), Bamboo)) return false;
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

    // Walk into the trigger (production path) and settle the wave.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        static double Start = 0;
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        APrototypeCharacter* P = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
        if (Start == 0) Start = FPlatformTime::Seconds();
        const bool bRunning = Session && Session->GetState() == ERoomSessionState::Running;
        if (!P || bRunning || FPlatformTime::Seconds() - Start > 4.0)
        {
            if (P) P->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            TestTrue(TEXT("the trigger overlap started the run"), bRunning);
            return true;
        }
        P->DevSetMoveAxes(1.f, 0.f);
        return false;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        TestTrue(TEXT("the wave birthed the two goblins"), []() {
            int32 Count = 0;
            for (TActorIterator<AGoblinThrowerEnemy> It(GWorld); It; ++It) ++Count;
            return Count == 2;
        }());
        return true;
    }));

    // The slice combat: close depth-first on the nearest alive goblin and
    // drive light_01 through the player's own combat chain until both are
    // dead and the wave settles. One long drive-until command (per-frame).
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        static double Start = 0;
        static bool bFirstHitObserved = false;
        static int32 AttackCount = 0;

        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        APrototypeCharacter* P = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
        if (Start == 0)
        {
            Start = FPlatformTime::Seconds();
            bFirstHitObserved = false;
            AttackCount = 0;
        }
        if (Session == nullptr || P == nullptr || FPlatformTime::Seconds() - Start > 90.0)
        {
            if (P) P->DevSetMoveAxes(0.f, 0.f);
            Start = 0;
            AddInfo(FString::Printf(TEXT("slice combat ended: attacks=%d firstHit=%d sessionState=%d killed=%d"),
                AttackCount, bFirstHitObserved ? 1 : 0,
                Session ? static_cast<int32>(Session->GetState()) : -1,
                Session ? Session->GetKilledCount() : -1));
            TestTrue(TEXT("the player's attack chain killed both goblins"),
                Session && Session->GetState() == ERoomSessionState::Cleared
                && Session->GetKilledCount() == 2);
            FScreenshotRequest::RequestScreenshot(Output / TEXT("slice-victory.png"), true, false);
            return true;
        }

        float Dx = 0.f, Dy = 0.f;
        AGoblinThrowerEnemy* Target = FindNearestAliveGoblin(GWorld, P->GetActorLocation(), Dx, Dy);
        if (Target == nullptr)
        {
            // The wave is dead; leave the settle to the next wait step.
            P->DevSetMoveAxes(0.f, 0.f);
            return false;
        }

        // Depth-first closing (the game's own movement rule), then attack.
        if (FMath::Abs(Dy) > 40.f)
        {
            P->DevSetMoveAxes(0.f, Dy > 0.f ? 1.f : -1.f);
            return false;
        }
        if (FMath::Abs(Dx) > 150.f)
        {
            P->DevSetMoveAxes(Dx > 0.f ? 1.f : -1.f, 0.f);
            return false;
        }
        P->DevSetMoveAxes(0.f, 0.f);

        // Face and strike through the production entry (the input buffer's
        // own destination). The component refuses while an instance runs;
        // Free ticks fire the next light attack.
        UCombatComponent* Combat = P->GetCombat();
        if (Combat != nullptr && Combat->GetSnapshot().ActionState == ECombatActionState::Free)
        {
            const int32 Facing = Dx >= 0.f ? 1 : -1;
            if (Combat->TryStartAttack(FName(TEXT("light_01")), Facing))
            {
                ++AttackCount;
            }
        }
        if (!bFirstHitObserved && Target->GetHealthComponent()->GetHealth()
            < Target->GetHealthComponent()->GetMaxHealth())
        {
            bFirstHitObserved = true;
            AddInfo(FString::Printf(TEXT("first player hit landed: goblin hp=%.0f/60 after %d attacks"),
                Target->GetHealthComponent()->GetHealth(), AttackCount));
            TestTrue(TEXT("the player's damage flows through the shared chain"),
                Target->GetHealthComponent()->GetHealth() > 0.f);
        }
        return false;
    }));
    // The wave settlement (the session's 1s wait) closes the room.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Output]()
    {
        URoomSessionSubsystem* Session = GWorld->GetSubsystem<URoomSessionSubsystem>();
        if (!TestNotNull(TEXT("session for the final state"), Session)) return true;
        TestTrue(TEXT("the slice settled into Cleared"),
            Session->GetState() == ERoomSessionState::Cleared);
        FScreenshotRequest::RequestScreenshot(Output / TEXT("slice-settled.png"), true, false);
        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
    return true;
}
#endif
