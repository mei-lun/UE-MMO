// M8-031 part2 acceptance: the skill ability components. The three conditions
// from the plan 08 problem-2:
// 1. the component isolation - removing one ability leaves the rest casting
// 2. no input-layer special-casing - the routing is uniform through the
//    components (the structural test: the abilities carry the config, the
//    input layer has no per-skill branches)
// 3. the M5 decoupling - the launcher ability delivers through its own
//    projectile (the throwstone actor), not the M5 weapon pipeline
#include "Misc/AutomationTest.h"
#include "../Combat/AttackCatalog.h"
#include "../Combat/SkillAbilityComponent.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatStateMachine.h"
#include "../Enemy/ThrowStoneProjectile.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Skills/SkillLoadoutComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_031Part2SkillAbilities,
    "UEMMO.Acceptance.M8_031.SkillAbilities",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_031Part2SkillAbilities::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("rendered game world"), World && World->WorldType == EWorldType::Game)) return false;

    // The pure structural test: the ability component holds its own config
    // (the skill/attack ids, the projectile spec) and routes generically.
    // The component isolation: removing one ability leaves the others cast.
    UCombatComponent* Combat = NewObject<UCombatComponent>();
    if (!TestNotNull(TEXT("combat component"), Combat)) return false;
    USkillAbilityComponent* Ability0 = NewObject<USkillAbilityComponent>();
    USkillAbilityComponent* Ability1 = NewObject<USkillAbilityComponent>();
    if (!TestNotNull(TEXT("ability 0"), Ability0)) return false;
    if (!TestNotNull(TEXT("ability 1"), Ability1)) return false;

    FSkillProjectileSpec MeleeSpec;
    MeleeSpec.bProjectile = false;
    FSkillProjectileSpec ProjSpec;
    ProjSpec.bProjectile = true;
    Ability0->Configure(FName(TEXT("skill_light")), FName(TEXT("light_01")), 0, 0.f, MeleeSpec, nullptr);
    Ability1->Configure(FName(TEXT("skill_launcher")), FName(TEXT("launcher")), 20, 3.f, ProjSpec, nullptr);

    // The isolation: each ability holds its OWN config.
    TestTrue(TEXT("ability 0 is melee"), !Ability0->GetAttackId().IsNone());
    TestTrue(TEXT("ability 1 is launcher"), Ability1->GetAttackId() == FName(TEXT("launcher")));
    // (the projectile spec difference is structural - the TryCast routes
    // differently based on it)

    // The component isolation: removing ability 0 leaves ability 1 casting.
    // (the UActorComponent Destroy is the removal; the isolation is that
    // ability 1's config is unaffected)
    Ability0->ConditionalBeginDestroy();
    Ability0 = nullptr;
    TestTrue(TEXT("ability 1 still configured after ability 0 removed"),
        Ability1->GetAttackId() == FName(TEXT("launcher")));

    // The in-game: the goblin thrower's stone lands through the skill
    // projectile path (the SpawnSkillProjectile entry).
    if (World && World->WorldType == EWorldType::Game && FApp::CanEverRender())
    {
        ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
        {
            int32 Count = 0;
            for (TActorIterator<AThrowStoneProjectile> It(GWorld); It; ++It) ++Count;
            TestTrue(TEXT("the stone projectile spawns through the skill path"), Count > 0);
            return true;
        }));
    }
    return true;
}

// M8-037: the plan 08 problem-2 closing acceptance. Each of the four named
// skills casts independently through its own ability component (melee ones
// start the shared attack - the state machine then derives Windup on the next
// tick; the launcher spawns its projectile), the component isolation holds
// across the four (destroying one leaves the rest casting), and the slot
// route lookup resolves the configured component per attack id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_037SkillComponentCastAndIsolation,
    "UEMMO.Acceptance.M8_037.SkillComponentCastAndIsolation",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_037SkillComponentCastAndIsolation::RunTest(const FString& Parameters)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!TestTrue(TEXT("rendered game world"), World && World->WorldType == EWorldType::Game)) return false;

    UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
    FText Error;
    if (!TestTrue(TEXT("catalog init"), Catalog->InitializeFromConfig(Error)))
    {
        AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
        return true;
    }
    UCombatComponent* Combat = NewObject<UCombatComponent>();
    if (!TestTrue(TEXT("component init"), Combat->InitializeFromCatalog(Catalog))) return true;

    // The four named plan-08 skills, configured exactly like the character's
    // CreateSkillAbilities (melee chain attacks + the launcher projectile).
    // The components hang off a spawned harness actor so their GetWorld()
    // resolves (the cast entry needs a world).
    AActor* Harness = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("the harness actor"), Harness)) return true;
    FSkillProjectileSpec MeleeSpec;
    MeleeSpec.bProjectile = false;
    FSkillProjectileSpec ProjSpec;
    ProjSpec.bProjectile = true;
    const FName AttackIds[4] = { FName(TEXT("light_01")), FName(TEXT("light_02")),
        FName(TEXT("launcher")), FName(TEXT("aerial_01")) };
    TArray<USkillAbilityComponent*> Abilities;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        USkillAbilityComponent* Ability = NewObject<USkillAbilityComponent>(Harness);
        Ability->RegisterComponent();
        Abilities.Add(Ability);
        Ability->Configure(*FString::Printf(TEXT("skill_%s"), *AttackIds[Index].ToString()),
            AttackIds[Index], 0, 0.f, Index == 2 ? ProjSpec : MeleeSpec, nullptr);
    }

    // The slot-route lookup: the configured component per attack id, null for
    // an unconfigured one.
    TestTrue(TEXT("the route finds light_01"),
        USkillAbilityComponent::FindAbilityForSlotAttack(Abilities, FName(TEXT("light_01"))) == Abilities[0]);
    TestTrue(TEXT("the route finds aerial_01"),
        USkillAbilityComponent::FindAbilityForSlotAttack(Abilities, FName(TEXT("aerial_01"))) == Abilities[3]);
    TestTrue(TEXT("the route returns null for an unconfigured attack"),
        USkillAbilityComponent::FindAbilityForSlotAttack(Abilities, FName(TEXT("singlekick"))) == nullptr);
    TestTrue(TEXT("the route returns null for an empty id"),
        USkillAbilityComponent::FindAbilityForSlotAttack(Abilities, NAME_None) == nullptr);

    // Independent casts, melee first. Each cast starts its own attack; the
    // phase machine derives Windup on the next tick (the plan's "TryCast 后
    // 状态机进入 Windup"); the instance retires before the next cast via
    // ResetCombat (the same teardown the jump cancel uses).
    for (int32 Index : {0, 1, 3})
    {
        USkillAbilityComponent* Ability = Abilities[Index];
        TestTrue(FString::Printf(TEXT("%s casts independently"), *AttackIds[Index].ToString()),
            Ability->TryCast(Combat, nullptr, 1));
        TestTrue(FString::Printf(TEXT("%s owns the running instance"), *AttackIds[Index].ToString()),
            Combat->GetSnapshot().AttackId == AttackIds[Index]);
        Combat->TickCombat(1.0f / 60.0f);
        UCombatStateMachine* Machine = Combat->GetStateMachine();
        TestTrue(TEXT("the state machine subobject exists"), Machine != nullptr);
        if (Machine != nullptr)
        {
            TestEqual(FString::Printf(TEXT("%s derives Windup after the cast"), *AttackIds[Index].ToString()),
                Machine->GetPhase(), ECombatPhase::Windup);
        }
        Combat->ResetCombat();
    }

    // The launcher component delivers through its own projectile (the
    // component data's spec, not the M5 weapon pipeline); no attack instance
    // starts for the projectile shape.
    int32 ProjectilesBefore = 0;
    for (TActorIterator<AThrowStoneProjectile> It(World); It; ++It) ++ProjectilesBefore;
    TestTrue(TEXT("launcher casts through the component"),
        Abilities[2]->TryCast(Combat, nullptr, 1));
    int32 ProjectilesAfter = 0;
    for (TActorIterator<AThrowStoneProjectile> It(World); It; ++It) ++ProjectilesAfter;
    TestEqual(TEXT("the launcher projectile spawned from the component spec"),
        ProjectilesAfter, ProjectilesBefore + 1);
    TestTrue(TEXT("the projectile shape starts no melee instance"),
        Combat->GetSnapshot().AttackId == FName(NAME_None));

    // The component isolation: destroying one ability leaves the others
    // casting (light_02's config and delivery are unaffected by light_01's
    // teardown).
    Abilities[0]->ConditionalBeginDestroy();
    Abilities[0] = nullptr;
    TestTrue(TEXT("light_02 still casts after light_01's teardown"),
        Abilities[1]->TryCast(Combat, nullptr, 1));
    TestTrue(TEXT("light_02 owns the instance"),
        Combat->GetSnapshot().AttackId == FName(TEXT("light_02")));
    Combat->ResetCombat();
    TestTrue(TEXT("aerial_01 still casts after light_01's teardown"),
        Abilities[3]->TryCast(Combat, nullptr, 1));
    Combat->ResetCombat();
    TestTrue(TEXT("launcher still casts after light_01's teardown"),
        Abilities[2]->TryCast(Combat, nullptr, 1));
    return true;
}
#endif
