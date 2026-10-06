// M5-015: the target-side reaction execution (interface contract sections 1
// and 5). Pins the stateful face of the hit-reaction pipeline that M5-014
// left with the legacy constants: the victim's explicitly overridden target
// policy wins over the attacker's injected member (per-target heavy/boss
// configuration), the poise pool gates control until it breaks (with the
// configured regeneration and the attack-side BypassPoise penetration), the
// unified target-side float-cycle launch count covers every component-bearing
// victim (wave enemies included), the bounded air-control window revokes the
// extra air control when the configured max air time expires (forced descent,
// never a teleport) and the airborne damage scale hook applies to airborne
// victims. The default policy keeps every legacy number byte for byte.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/ReactionResolver.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// Compile-time pins: the new state facts stay value inputs on the context and
// the depletion report stays a value on the request (interface contract 0.5).
static_assert(!std::is_pointer_v<decltype(FReactionHitContext::TargetPoiseCurrent)>, "FReactionHitContext::TargetPoiseCurrent must stay a value type");
static_assert(!std::is_pointer_v<decltype(FReactionHitContext::bTargetAirControlExpired)>, "FReactionHitContext::bTargetAirControlExpired must stay a value type");
static_assert(!std::is_pointer_v<decltype(FHitReactionRequest::PoiseDepletion)>, "FHitReactionRequest::PoiseDepletion must stay a value type");

namespace UE::UEMMO::Tasks::M5_015
{
	// ------------------------------------------------------------------
	// Component-level scene (the M5-013/M5-014 temp world + real ini
	// catalog mode, duplicated per test file: bUseUnity=false keeps the
	// copies independent translation units).
	// ------------------------------------------------------------------

	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is the attacker's feet origin.
	const FVector M5_015_SceneBase(74000.0, 56000.0, 600.0);

	// Half extent of one test body box; sits fully inside the shared hit box
	// of all four legacy attacks (offset (95,0,90), half extent (85,50,70)).
	const FVector M5_015_BodyHalfExtent(20.0, 20.0, 95.0);

	// World acquisition, M1-019 pattern: a private temp world, GWorld fallback.
	static UWorld* M5_015_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_015_TestWorld")));
		if (TempWorld != nullptr)
		{
			return TempWorld;
		}
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_015_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_015_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	// Loads the real read-only catalog from Config/DefaultGame.ini.
	static UAttackCatalog* M5_015_NewCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("the attack catalog initializes from DefaultGame.ini"), Catalog->InitializeFromConfig(Error)))
		{
			Test.AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
			return nullptr;
		}
		return Catalog;
	}

	// Counts HitConfirmed broadcasts and keeps the last payload.
	struct FM5_015_HitEvents
	{
		int32 HitCount = 0;
		FCombatHit LastHit;

		void Bind(UCombatComponent& Component)
		{
			Component.OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				++HitCount;
				LastHit = Hit;
			});
		}
	};

	/**
	 * One scene at its own remote base: the real catalog, one attacker box
	 * actor with a combat component whose feet sit at a fixed injected
	 * origin, and helpers to spawn box victims and enemies at the shared hit
	 * box center (all four legacy attacks share one box shape).
	 */
	struct FM5_015_Scene
	{
		UAttackCatalog* Catalog = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		FM5_015_HitEvents Events;
		FVector SceneBase = FVector::ZeroVector;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			SceneBase = InBase;
			Catalog = M5_015_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}

			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), SceneBase, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M5_015_AttackerBody"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M5_015_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(SceneBase);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M5_015_Combat"));
			if (!Test.TestTrue(TEXT("the attacker combat component attaches the catalog"),
				Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->RegisterComponent();
			Combat->SetFeetLocationProvider([FixedFeet = SceneBase]() { return FixedFeet; });
			Events.Bind(*Combat);
			return true;
		}

		const UAttackDefinition* FindDefinition(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			Test.TestTrue(FString::Printf(TEXT("the catalog holds %s"), AttackId), Definition != nullptr);
			return Definition;
		}

		// Spawns a box victim (health, optionally a victim combat component)
		// at the shared hit box center of the requested attack/facing pair.
		AActor* SpawnBoxTarget(UWorld& World, const UAttackDefinition& Definition, int32 Facing, bool bWithCombat = false)
		{
			const FVector BoxCenter = ComputeHitBox(SceneBase, Facing, Definition).Center;
			FActorSpawnParameters Params;
			AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), BoxCenter, FRotator::ZeroRotator, Params);
			if (Actor == nullptr)
			{
				return nullptr;
			}
			UBoxComponent* TargetBody = NewObject<UBoxComponent>(Actor, TEXT("M5_015_TargetBody"));
			Actor->SetRootComponent(TargetBody);
			TargetBody->SetMobility(EComponentMobility::Movable);
			TargetBody->SetBoxExtent(M5_015_BodyHalfExtent);
			TargetBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			TargetBody->SetCollisionObjectType(ECC_Pawn);
			TargetBody->SetCollisionResponseToAllChannels(ECR_Ignore);
			TargetBody->RegisterComponent();
			TargetBody->SetWorldLocation(BoxCenter);

			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_015_Health"));
			Health->RegisterComponent();
			if (bWithCombat)
			{
				UCombatComponent* VictimCombat = NewObject<UCombatComponent>(Actor, TEXT("M5_015_VictimCombat"));
				VictimCombat->RegisterComponent();
			}
			return Actor;
		}

		// Spawns a training enemy at the hit box center for the requested
		// facing (the M1-022 pattern: capsule center at the box center, the
		// movement component activated for launches).
		ATrainingEnemy* SpawnTrainingEnemy(UWorld& World, const UAttackDefinition& Definition, int32 Facing)
		{
			const FVector BoxCenter = ComputeHitBox(SceneBase, Facing, Definition).Center;
			FActorSpawnParameters SpawnParams;
			ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
				ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, SpawnParams);
			if (Enemy == nullptr)
			{
				return nullptr;
			}
			if (!Enemy->HasActorBegunPlay())
			{
				Enemy->DispatchBeginPlay();
			}
			UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			if (EnemyMovement != nullptr)
			{
				if (!EnemyMovement->IsActive())
				{
					EnemyMovement->Activate(/*bReset*/ true);
				}
				if (EnemyMovement->MovementMode == MOVE_None)
				{
					EnemyMovement->SetDefaultMovementMode();
				}
			}
			return Enemy;
		}

		static float TargetHealth(AActor& Target)
		{
			const UHealthComponent* Health = Target.FindComponentByClass<UHealthComponent>();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		static UCombatComponent* TargetCombat(AActor& Target)
		{
			return Target.FindComponentByClass<UCombatComponent>();
		}

		// Retires a case target as a corpse (a dead combatant is never a
		// query target again; the temp world has no world context, so
		// DestroyActor is not an option - the M5-013 lesson).
		static void RetireTarget(AActor* Target)
		{
			if (Target != nullptr)
			{
				if (UHealthComponent* Health = Target->FindComponentByClass<UHealthComponent>())
				{
					Health->ApplyDamage(Health->GetMaxHealth());
				}
			}
		}

		// Advances the attacker component by exactly Count single 1/60 s frames.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Combat->TickCombat(1.0f / 60.0f);
			}
		}

		// Starts one attack instance and ticks exactly to its first active
		// frame, where the hit lands.
		bool HitOnce(FAutomationTestBase& Test, const TCHAR* AttackId, int32 Facing)
		{
			const UAttackDefinition* Definition = FindDefinition(Test, AttackId);
			if (Definition == nullptr)
			{
				return false;
			}
			if (!Test.TestTrue(FString::Printf(TEXT("%s starts"), AttackId),
				Combat->TryStartAttack(FName(AttackId), Facing)))
			{
				return false;
			}
			TickFrames(Definition->ActiveWindow.StartFrame + 1);
			return true;
		}

		// Ticks the running instance past its final frame so the attacker is
		// Free again (and the shot's ledger keys release).
		void FinishInstance(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = FindDefinition(Test, AttackId);
			if (Definition != nullptr)
			{
				TickFrames(Definition->DurationFrames + 2);
			}
		}

		// Advances the victim's injected input clock by the given seconds and
		// ticks it once (the stun deadline/poise regen/air window read the
		// victim's own injected clock).
		static void AdvanceVictimClock(UCombatComponent& VictimCombat, double DeltaSeconds)
		{
			VictimCombat.SetInputClockSeconds(VictimCombat.GetInputClockSeconds() + DeltaSeconds);
			VictimCombat.TickCombat(1.0f / 60.0f);
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_015;

// ---------------------------------------------------------------------------
// VictimPolicyOverrideDrivesHeavyAndBossRefusal
// ---------------------------------------------------------------------------

// The victim's explicitly overridden target policy wins over the attacker's
// injected member: a heavy victim (launch-gated with a poise pool) takes the
// launcher damage but no launch and no stun (the pool holds), and a boss
// victim (blanket control immunity, death resistant) takes full damage with
// every control refused - per-target configuration, zero class-code change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015VictimPolicyOverrideDrivesHeavyAndBossRefusal,
	"UEMMO.Tasks.M5_015.VictimPolicyOverrideDrivesHeavyAndBossRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015VictimPolicyOverrideDrivesHeavyAndBossRefusal::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. The heavy victim: the launch gate refuses the launch and the poise
	//    pool holds the stun; the damage stands and depletes the pool.
	{
		FM5_015_Scene Scene;
		if (!Scene.Build(*this, *World, M5_015_SceneBase))
		{
			return true;
		}
		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
		if (Definition == nullptr)
		{
			return true;
		}
		AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, /*Facing*/ 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the heavy case spawns its victim"), Target))
		{
			return true;
		}
		UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Target);
		if (!TestNotNull(TEXT("precondition: the heavy victim carries a combat component"), VictimCombat))
		{
			return true;
		}
		FTargetReaction Heavy = MakeLegacyNormalTargetReaction();
		Heavy.PolicyId = TEXT("m5_015_heavy");
		Heavy.bAllowLaunch = false;
		Heavy.PoiseMax = 120.0f;
		Heavy.PoiseRegenSeconds = 2.0f;
		VictimCombat->SetTargetReactionPolicy(Heavy);

		if (!Scene.HitOnce(*this, TEXT("launcher"), /*Facing*/ 1))
		{
			return true;
		}
		TestEqual("the heavy victim takes the launcher damage", FM5_015_Scene::TargetHealth(*Target), 82.0f);
		TestTrue("the heavy victim accepts no launch (the impulse carries no Z)",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 0.0f), 1.0e-4f));
		TestTrue("the heavy victim accepts no stun while the pool holds (stays Free)",
			VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		TestEqual("the launcher damage depletes the heavy pool by its resolved damage",
			VictimCombat->GetEffectivePoiseCurrent(), 102.0f);
		FM5_015_Scene::RetireTarget(Target);
	}

	// 2. The boss victim: the blanket control immunity refuses every control
	//    kind before the pool is even consulted (no depletion); the damage
	//    stands and death resistance is configuration the entry already owns.
	{
		FM5_015_Scene Scene;
		if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(3000.0, 0.0, 0.0)))
		{
			return true;
		}
		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
		if (Definition == nullptr)
		{
			return true;
		}
		AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the boss case spawns its victim"), Target))
		{
			return true;
		}
		UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Target);
		if (!TestNotNull(TEXT("precondition: the boss victim carries a combat component"), VictimCombat))
		{
			return true;
		}
		FTargetReaction Boss = MakeLegacyNormalTargetReaction();
		Boss.PolicyId = TEXT("m5_015_boss");
		Boss.bImmuneControl = true;
		Boss.bDeathResistant = true;
		Boss.PoiseMax = 999.0f;
		VictimCombat->SetTargetReactionPolicy(Boss);

		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the boss victim takes the full light_01 damage", FM5_015_Scene::TargetHealth(*Target), 90.0f);
		TestTrue("the boss victim stays Free (no stun, no knockdown)",
			VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		TestEqual("the control-immune victim spends no poise (the gate refuses before the pool)",
			VictimCombat->GetEffectivePoiseCurrent(), 999.0f);
		FM5_015_Scene::RetireTarget(Target);
	}
	return true;
}

// ---------------------------------------------------------------------------
// PoisePoolGatesControlUntilBreak
// ---------------------------------------------------------------------------

// A poise-pool victim (25, no regeneration) holds every control kind while
// the pool survives the hit's resolved damage and breaks exactly when a hit
// depletes it: light_01 (10 damage) holds on the first two hits, breaks on
// the third (the stun lands) and the break resets the pool to full, so the
// fourth hit is held again. Damage and dedup stand on every hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015PoisePoolGatesControlUntilBreak,
	"UEMMO.Tasks.M5_015.PoisePoolGatesControlUntilBreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015PoisePoolGatesControlUntilBreak::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(6000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
	if (Definition == nullptr)
	{
		return true;
	}
	AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the poise case spawns its victim"), Target))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Target);
	if (!TestNotNull(TEXT("precondition: the poise victim carries a combat component"), VictimCombat))
	{
		return true;
	}
	FTargetReaction PoiseTarget = MakeLegacyNormalTargetReaction();
	PoiseTarget.PolicyId = TEXT("m5_015_poise_25");
	PoiseTarget.PoiseMax = 25.0f;
	VictimCombat->SetTargetReactionPolicy(PoiseTarget);

	// Hit 1: the pool holds (25 - 10 = 15), the stun is refused, damage lands.
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestEqual("the held hit still removes its damage", FM5_015_Scene::TargetHealth(*Target), 90.0f);
	TestTrue("the pool holds the first hit's stun (victim stays Free)",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual("the first hit depletes the pool to 15", VictimCombat->GetEffectivePoiseCurrent(), 15.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));

	// Hit 2: the pool holds again (15 - 10 = 5).
	FM5_015_Scene::AdvanceVictimClock(*VictimCombat, 0.3);
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestTrue("the pool holds the second hit's stun (victim stays Free)",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual("the second hit depletes the pool to 5", VictimCombat->GetEffectivePoiseCurrent(), 5.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));

	// Hit 3: the hit depletes the pool below zero - the break admits the stun.
	FM5_015_Scene::AdvanceVictimClock(*VictimCombat, 0.3);
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestTrue("the breaking hit stuns the victim",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestEqual("the break resets the pool to full", VictimCombat->GetEffectivePoiseCurrent(), 25.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));

	// Hit 4 (after the stun expired): the fresh pool holds again.
	FM5_015_Scene::AdvanceVictimClock(*VictimCombat, 1.0);
	TestTrue("precondition: the stun expired back to Free",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestTrue("the reset pool holds the fourth hit's stun (victim back to Free)",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual("the fourth hit depletes the fresh pool to 15", VictimCombat->GetEffectivePoiseCurrent(), 15.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));
	return true;
}

// ---------------------------------------------------------------------------
// PoiseRegenRestoresThePool
// ---------------------------------------------------------------------------

// The configured poise regeneration period restores the pool between hits:
// with PoiseRegenSeconds 2.0 a pool pressured at t=1.0 reads full again at
// t=3.5, so the hold/break cadence stretches (every spaced hit faces a full
// pool; only back-to-back pressure accumulates).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015PoiseRegenRestoresThePool,
	"UEMMO.Tasks.M5_015.PoiseRegenRestoresThePool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015PoiseRegenRestoresThePool::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(9000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
	if (Definition == nullptr)
	{
		return true;
	}
	AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the regen case spawns its victim"), Target))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Target);
	if (!TestNotNull(TEXT("precondition: the regen victim carries a combat component"), VictimCombat))
	{
		return true;
	}
	FTargetReaction RegenTarget = MakeLegacyNormalTargetReaction();
	RegenTarget.PolicyId = TEXT("m5_015_poise_regen");
	RegenTarget.PoiseMax = 25.0f;
	RegenTarget.PoiseRegenSeconds = 2.0f;
	VictimCombat->SetTargetReactionPolicy(RegenTarget);
	VictimCombat->SetInputClockSeconds(1.0);

	// Hit 1 at t=1.0: the pool holds (15).
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestEqual("the first hit depletes the pool to 15", VictimCombat->GetEffectivePoiseCurrent(), 15.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));

	// Hit 2 at t=3.5 (2.5 s after the pressure): the pool regenerated to full,
	// the hit depletes it to 15 again - no accumulation across the period.
	FM5_015_Scene::AdvanceVictimClock(*VictimCombat, 2.5);
	TestEqual("the pool regenerated to full before the second hit",
		VictimCombat->GetEffectivePoiseCurrent(), 25.0f);
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestEqual("the second hit depletes the regenerated pool to 15",
		VictimCombat->GetEffectivePoiseCurrent(), 15.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));

	// Hit 3 at t=4.0 (0.5 s after the pressure): no regeneration yet - the
	// pool reads 5, and hit 4 in the same window breaks it.
	FM5_015_Scene::AdvanceVictimClock(*VictimCombat, 0.5);
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestEqual("the third hit depletes the pool to 5 (inside the regen period)",
		VictimCombat->GetEffectivePoiseCurrent(), 5.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestTrue("the fourth hit breaks the pool and stuns the victim",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Scene.FinishInstance(*this, TEXT("light_01"));
	return true;
}

// ---------------------------------------------------------------------------
// BypassPoisePenetratesThePool
// ---------------------------------------------------------------------------

// The attack side's BypassPoise penetration skips the victim's poise
// threshold: the very first light_01 stuns a full-pool victim, and the
// penetration does not grind the pool (it stays untouched at full).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015BypassPoisePenetratesThePool,
	"UEMMO.Tasks.M5_015.BypassPoisePenetratesThePool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015BypassPoisePenetratesThePool::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(12000.0, 0.0, 0.0)))
	{
		return true;
	}
	FAttackReaction Bypass;
	Bypass.ReactionId = TEXT("m5_015_bypass");
	Bypass.ControlPenetration = EControlPenetration::BypassPoise;
	Scene.Combat->SetAttackReaction(Bypass);

	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
	if (Definition == nullptr)
	{
		return true;
	}
	AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the bypass case spawns its victim"), Target))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Target);
	if (!TestNotNull(TEXT("precondition: the bypass victim carries a combat component"), VictimCombat))
	{
		return true;
	}
	FTargetReaction PoiseTarget = MakeLegacyNormalTargetReaction();
	PoiseTarget.PolicyId = TEXT("m5_015_bypass_pool");
	PoiseTarget.PoiseMax = 25.0f;
	VictimCombat->SetTargetReactionPolicy(PoiseTarget);

	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestTrue("the penetrating hit stuns the full-pool victim immediately",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestEqual("the penetrating hit still removes its damage", FM5_015_Scene::TargetHealth(*Target), 90.0f);
	TestEqual("the penetration leaves the pool untouched", VictimCombat->GetEffectivePoiseCurrent(), 25.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));
	return true;
}

// ---------------------------------------------------------------------------
// PoiseDepletesOncePerAcceptedShot
// ---------------------------------------------------------------------------

// The poise pressure is recorded exactly once per accepted hit submission:
// a launcher against a 25-pool victim holds its launch (depletion 18, no Z),
// and the second launcher instance breaks the pool (the launch lands, the
// cycle counts it). A duplicate shot submission is refused by the unified
// ledger before any pressure is recorded (the M5-010 dedup face), so one
// shot can never deplete twice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015PoiseDepletesOncePerAcceptedShot,
	"UEMMO.Tasks.M5_015.PoiseDepletesOncePerAcceptedShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015PoiseDepletesOncePerAcceptedShot::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(15000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
	if (Definition == nullptr)
	{
		return true;
	}
	AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the depletion case spawns its victim"), Target))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Target);
	if (!TestNotNull(TEXT("precondition: the depletion victim carries a combat component"), VictimCombat))
	{
		return true;
	}
	FTargetReaction PoiseTarget = MakeLegacyNormalTargetReaction();
	PoiseTarget.PolicyId = TEXT("m5_015_depletion_pool");
	PoiseTarget.PoiseMax = 25.0f;
	VictimCombat->SetTargetReactionPolicy(PoiseTarget);

	// Instance 1: the pool holds the launch (25 - 18 = 7) - damage lands,
	// no launch Z, no cycle count, no accepted ledger control.
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	TestEqual("the held launcher still removes its damage", FM5_015_Scene::TargetHealth(*Target), 82.0f);
	TestTrue("the held launcher carries no launch Z",
		Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 0.0f), 1.0e-4f));
	TestEqual("the held launcher depletes the pool to 7", VictimCombat->GetEffectivePoiseCurrent(), 7.0f);
	TestEqual("the held launcher counted no cycle launch", VictimCombat->GetLaunchCycleCount(), 0);
	TestEqual("the held launcher recorded no accepted ledger control",
		Scene.Combat->GetUnifiedLedgerAcceptedControlCount(), 0);
	Scene.FinishInstance(*this, TEXT("launcher"));

	// Instance 2: the depletion breaks the pool - the launch lands and the
	// cycle counts it (the launch admission reads the unified count).
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	TestTrue("the breaking launcher carries the full launch Z",
		Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 700.0f), 1.0e-4f));
	TestEqual("the break reset the pool to full", VictimCombat->GetEffectivePoiseCurrent(), 25.0f);
	TestEqual("the admitted launch is counted once", VictimCombat->GetLaunchCycleCount(), 1);
	Scene.FinishInstance(*this, TEXT("launcher"));
	return true;
}

// ---------------------------------------------------------------------------
// LandingAndResetClearCyclePoiseAndAirWindow
// ---------------------------------------------------------------------------

// The float-cycle count, the air-control window and the poise pool are the
// victim component's execution state with explicit clear points: a ground
// contact reopens the cycle and closes the window, a ResetCombat clears all
// three (poise back to full), and death clears them the same way. Nothing
// survives into a fresh life.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015LandingAndResetClearCyclePoiseAndAirWindow,
	"UEMMO.Tasks.M5_015.LandingAndResetClearCyclePoiseAndAirWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015LandingAndResetClearCyclePoiseAndAirWindow::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(18000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
	if (Definition == nullptr)
	{
		return true;
	}
	ATrainingEnemy* Enemy = Scene.SpawnTrainingEnemy(*World, *Definition, 1);
	if (!TestNotNull(TEXT("the clear case spawns its training enemy"), Enemy))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Enemy);
	if (!TestNotNull(TEXT("precondition: the training enemy carries a combat component"), VictimCombat))
	{
		return true;
	}

	// 1. A launch opens the cycle and the air window (unified count: the
	//    component API reads through to the enemy's count). The victim still
	//    carries its default pool-free policy here - the launch is admitted.
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	TestEqual("the launch is counted into the unified cycle", VictimCombat->GetLaunchCycleCount(), 1);
	TestEqual("the unified count reads the enemy's legacy count too", Enemy->GetLauncherCycleCount(), 1);
	Scene.FinishInstance(*this, TEXT("launcher"));

	// 2. A ground contact reopens the cycle and closes the air window: after
	//    the window's max air time passed, the expired fact still reads false.
	VictimCombat->OnGroundContact();
	VictimCombat->SetInputClockSeconds(10.0);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestEqual("the ground contact reopened the cycle", VictimCombat->GetLaunchCycleCount(), 0);
	TestFalse("the closed window never reports expired", VictimCombat->IsAirControlExpired());

	// 3. ResetCombat clears everything: a pressured poise pool reads full
	//    again and no stale window survives.
	VictimCombat->SetTargetReactionPolicy([]()
	{
		FTargetReaction Pool = MakeLegacyNormalTargetReaction();
		Pool.PolicyId = TEXT("m5_015_clear_pool");
		Pool.PoiseMax = 25.0f;
		return Pool;
	}());
	VictimCombat->RecordPoisePressure(10.0f);
	TestEqual("precondition: the pressure depleted the pool", VictimCombat->GetEffectivePoiseCurrent(), 15.0f);
	VictimCombat->ResetCombat();
	TestEqual("the reset restored the poise pool to full", VictimCombat->GetEffectivePoiseCurrent(), 25.0f);
	TestEqual("the reset reopened the cycle", VictimCombat->GetLaunchCycleCount(), 0);
	VictimCombat->SetInputClockSeconds(100.0);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestFalse("the reset closed the air window", VictimCombat->IsAirControlExpired());

	// 4. Death clears the same state: the corpse carries no live window.
	VictimCombat->RecordPoisePressure(10.0f);
	VictimCombat->SetDead(true);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestEqual("death restored the poise pool", VictimCombat->GetEffectivePoiseCurrent(), 25.0f);
	TestEqual("death reopened the cycle", VictimCombat->GetLaunchCycleCount(), 0);
	VictimCombat->SetInputClockSeconds(200.0);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestFalse("death closed the air window", VictimCombat->IsAirControlExpired());
	return true;
}

// ---------------------------------------------------------------------------
// MaxAirTimeRevokesAirControlAndForcesDescent
// ---------------------------------------------------------------------------

// The bounded air-control window: a launched victim whose max air time (the
// policy's 2.5 s default, tightened here) expired loses its extra air
// control - the next launcher deals its damage but admits no launch - and
// the victim's own tick forces the descent by clamping the upward velocity
// (never a teleport: the position stays). A ground contact reopens both the
// window and the cycle, so the next launcher rises at full speed again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015MaxAirTimeRevokesAirControlAndForcesDescent,
	"UEMMO.Tasks.M5_015.MaxAirTimeRevokesAirControlAndForcesDescent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015MaxAirTimeRevokesAirControlAndForcesDescent::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(21000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
	if (Definition == nullptr)
	{
		return true;
	}
	ATrainingEnemy* Enemy = Scene.SpawnTrainingEnemy(*World, *Definition, 1);
	if (!TestNotNull(TEXT("the air-time case spawns its training enemy"), Enemy))
	{
		return true;
	}

	// Launch 1: the full 700, the cycle counts it, the air window opens.
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Enemy);
	if (!TestNotNull(TEXT("precondition: the training enemy carries a combat component"), VictimCombat))
	{
		return true;
	}
	TestEqual("the first launch is counted", VictimCombat->GetLaunchCycleCount(), 1);
	TestFalse("the fresh air window is not expired", VictimCombat->IsAirControlExpired());
	Scene.FinishInstance(*this, TEXT("launcher"));

	// Simulate the physics steps the temp world never runs: the pending
	// launch applies absolutely on the next movement update, and the victim
	// is Falling with the launched upward velocity.
	UCharacterMovementComponent* VictimMovement = Enemy->GetCharacterMovement();
	if (!TestNotNull(TEXT("precondition: the victim carries a movement component"), VictimMovement))
	{
		return true;
	}
	VictimMovement->SetMovementMode(MOVE_Falling);
	VictimMovement->Velocity.Z = FMath::Max(VictimMovement->Velocity.Z, VictimMovement->PendingLaunchVelocity.Z);
	VictimMovement->PendingLaunchVelocity = FVector::ZeroVector;
	TestTrue("precondition: the launched victim rises", VictimMovement->Velocity.Z > 0.0f);
	const FVector PositionBeforeDescent = Enemy->GetActorLocation();

	// The window expires on the victim's own clock; the victim's tick forces
	// the descent (clamps the upward velocity, moves nothing).
	VictimCombat->SetInputClockSeconds(3.0);
	TestTrue("the expired window reports the air control revoked", VictimCombat->IsAirControlExpired());
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the forced descent clamped the upward velocity", VictimMovement->Velocity.Z <= 0.0f);
	TestTrue("the forced descent never teleports (position unchanged)",
		Enemy->GetActorLocation().Equals(PositionBeforeDescent, 1.0e-4f));

	// Launcher 2: damage stands, the launch is revoked, the cycle stays.
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	TestEqual("the revoked launcher still removes its damage", Enemy->GetHealthComponent()->GetHealth(), 64.0f);
	TestTrue("the revoked launcher carries no launch Z",
		Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 0.0f), 1.0e-4f));
	TestEqual("the revoked launch is not counted", VictimCombat->GetLaunchCycleCount(), 1);
	Scene.FinishInstance(*this, TEXT("launcher"));

	// A ground contact reopens the window and the cycle: launcher 3 rises at
	// the full definition speed again.
	VictimCombat->OnGroundContact();
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	TestTrue("the reopened cycle rises at the full 700",
		Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 700.0f), 1.0e-4f));
	TestEqual("the reopened launch is counted", VictimCombat->GetLaunchCycleCount(), 1);
	Scene.FinishInstance(*this, TEXT("launcher"));
	return true;
}

// ---------------------------------------------------------------------------
// AirborneDamageScaleHookApplies
// ---------------------------------------------------------------------------

// The airborne damage scale hook: a victim configured with a 2.0 scale takes
// the legacy numbers while grounded (10, 18) and double damage while
// airborne (the aerial follow-up 12 -> 24). The default scale 1.0 keeps
// every legacy number byte for byte (the whole legacy suite pins that).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_015AirborneDamageScaleHookApplies,
	"UEMMO.Tasks.M5_015.AirborneDamageScaleHookApplies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_015AirborneDamageScaleHookApplies::RunTest(const FString& Parameters)
{
	UWorld* World = M5_015_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_015_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_015_Scene Scene;
	if (!Scene.Build(*this, *World, M5_015_SceneBase + FVector(24000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Light = Scene.FindDefinition(*this, TEXT("light_01"));
	const UAttackDefinition* Launcher = Scene.FindDefinition(*this, TEXT("launcher"));
	if (Light == nullptr || Launcher == nullptr)
	{
		return true;
	}
	// All four legacy attacks share one hit box shape: one victim position
	// serves the whole sequence.
	ATrainingEnemy* Enemy = Scene.SpawnTrainingEnemy(*World, *Light, 1);
	if (!TestNotNull(TEXT("the scale case spawns its training enemy"), Enemy))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_015_Scene::TargetCombat(*Enemy);
	if (!TestNotNull(TEXT("precondition: the scale victim carries a combat component"), VictimCombat))
	{
		return true;
	}
	VictimCombat->SetAirborneDamageScale(2.0f);

	// The temp world never steps the movement component; pin the grounded
	// phase explicitly so the airborne check reads deterministically.
	if (UCharacterMovementComponent* ScaleMovement = Enemy->GetCharacterMovement())
	{
		ScaleMovement->SetMovementMode(MOVE_Walking);
	}

	// Grounded light_01: the legacy 10 (the scale does not touch the ground).
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestEqual("the grounded light_01 keeps the legacy 10", Enemy->GetHealthComponent()->GetHealth(), 90.0f);
	Scene.FinishInstance(*this, TEXT("light_01"));

	// The launcher lifts the victim at the legacy 18.
	if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	TestEqual("the grounded launcher keeps the legacy 18", Enemy->GetHealthComponent()->GetHealth(), 72.0f);
	Scene.FinishInstance(*this, TEXT("launcher"));

	// Simulate the physics step: the victim is Falling (airborne).
	UCharacterMovementComponent* VictimMovement = Enemy->GetCharacterMovement();
	if (!TestNotNull(TEXT("precondition: the scale victim carries a movement component"), VictimMovement))
	{
		return true;
	}
	VictimMovement->SetMovementMode(MOVE_Falling);

	// The aerial follow-up on the airborne victim: 12 * 2.0 = 24.
	if (!Scene.HitOnce(*this, TEXT("aerial_01"), 1))
	{
		return true;
	}
	TestEqual("the airborne aerial follow-up applies the configured scale (24)",
		Scene.Events.LastHit.Damage, 24.0f);
	TestEqual("the airborne victim took 10 + 18 + 24 total", Enemy->GetHealthComponent()->GetHealth(), 48.0f);
	Scene.FinishInstance(*this, TEXT("aerial_01"));
	return true;
}

#endif
