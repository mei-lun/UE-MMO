// M8-038: the plan 08 problem-1 display subscription acceptance. The display
// layer reacts to the UCombatStateMachine::OnPhaseChanged events instead of
// reverse-deriving the phase from the snapshot:
// - the goblin's display action flips to the damage hold the moment the hit
//   stun phase fires (event-driven, no per-tick re-derivation delay);
// - the player presentation's presented attack instance is torn down
//   synchronously with the Idle phase event after a mid-attack ResetCombat
//   (the interrupt/teardown path the card comment reserved for events).
#include "Misc/AutomationTest.h"
#include "../Enemy/GoblinThrowerEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_038PhaseDrivenDisplay,
    "UEMMO.Acceptance.M8_038.PhaseDrivenDisplay",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_038PhaseDrivenDisplay::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("game world"),
        World && World->WorldType == EWorldType::Game)) return false;

    // ---- Part A: the goblin's display action flips on the hit-stun EVENT ----
    APrototypeCharacter* PlayerChar = Cast<APrototypeCharacter>(
        UGameplayStatics::GetPlayerCharacter(World, 0));
    if (!TestNotNull(TEXT("player exists"), PlayerChar)) return false;
    const FVector Loc = PlayerChar->GetActorLocation() + FVector(200.f, 0.f, 50.f);
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    AGoblinThrowerEnemy* G = World->SpawnActor<AGoblinThrowerEnemy>(
        AGoblinThrowerEnemy::StaticClass(), Loc, FRotator::ZeroRotator, Params);
    if (!TestNotNull(TEXT("goblin spawned"), G)) return false;
    G->RefreshGoblinMaterial();
    if (!TestTrue(TEXT("the goblin visual is active"),
        G->GetGoblinVisual() != nullptr && G->GetGoblinVisual()->IsVisualActive())) return false;
    TestTrue(TEXT("the goblin idle display is stay"),
        G->GetGoblinVisual()->GetCurrentActionId() == FName(TEXT("stay")));

    // A real victim hit: the unified chain's NotifyHitReceived carries the
    // stun request (a raw Health::ApplyDamage bypasses the attack stun, so
    // the test drives the same entry the live attacker path uses); the
    // HitStun phase event flips the display to the damage hold. The subscribe
    // contract is the SAME tick (the machine broadcasts inside TickCombat's
    // derivation); a one-frame world step settles it deterministically.
    FCombatHit StunHit;
    StunHit.Target = G;
    StunHit.StunSeconds = 0.22f;
    G->GetCombatComponent()->NotifyHitReceived(StunHit);
    if (!TestTrue(TEXT("the goblin is stunned"),
        G->GetCombatComponent()->GetSnapshot().ActionState == ECombatActionState::HitStun)) return false;
    World->Tick(ELevelTick::LEVELTICK_All, 1.0f / 60.0f);
    const FName StunAction = G->GetGoblinVisual()->GetCurrentActionId();
    TestTrue(TEXT("the hit-stun event drove the damage hold display"),
        StunAction == FName(TEXT("damage_0")) || StunAction == FName(TEXT("damage_1")));

    // ---- Part B: the presented attack tears down with the Idle EVENT ----
    UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
    FText Error;
    if (!TestTrue(TEXT("catalog init"), Catalog->InitializeFromConfig(Error))) return false;
    UCombatComponent* Combat = NewObject<UCombatComponent>();
    if (!TestTrue(TEXT("combat init"), Combat->InitializeFromCatalog(Catalog))) return false;
    AActor* Harness = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("the presenter harness"), Harness)) return false;
    UCombatPresentationComponent* Presenter = NewObject<UCombatPresentationComponent>(Harness);
    Presenter->RegisterComponent();
    Presenter->SetSources(Combat, nullptr);

    TestTrue(TEXT("the probe attack starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Combat->TickCombat(1.0f / 60.0f);
    TestNotEqual(TEXT("the attack presentation is live"), Presenter->GetPresentedInstanceId(), uint64(0));

    // The mid-attack teardown: the machine's Idle event (M8-034's forced
    // reset) fires inside the same TickCombat call and the presentation
    // clears synchronously - no one-tick fallback delay.
    Combat->ResetCombat();
    Combat->TickCombat(1.0f / 60.0f);
    TestEqual(TEXT("the Idle event tore the presentation down synchronously"),
        Presenter->GetPresentedInstanceId(), uint64(0));
    return true;
}
#endif
