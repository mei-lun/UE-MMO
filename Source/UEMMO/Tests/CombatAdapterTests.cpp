// M5-013: the legacy melee adaptation onto the unified hit entry. Pins that
// the four legacy attacks (light_01/light_02/launcher/aerial_01) applied
// through the real CombatComponent active-window pipeline keep their exact
// M1-019 numbers (10/14/18/12), that the dedup/refusal/interrupt semantics
// are unchanged, and that the per-component unified ledger (M5-010) is the
// one dedup face the hits flow through - the production path for the player
// AND the melee enemies is the same single pipeline, with no second
// direct-to-health path left.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/MeleeEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_013
{
	// Remote base so the temp world can never collide with arena content
	// (the M1-019 precedent). X horizontal, Y depth, Z height.
	const FVector M5_013_SceneBase(42000.0, 46000.0, 600.0);

	// Half extent of one test body box; small enough to sit fully inside the
	// light_01 hit box (half extent (85,50,70) from the attack definition).
	const FVector M5_013_BodyHalfExtent(20.0, 20.0, 30.0);

	// World acquisition, M1-019 pattern: a private temp world, GWorld fallback.
	static UWorld* M5_013_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_013_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_013 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_013 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_013_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_013_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	// Spawns a plain actor with one query-enabled box root at Location.
	// bWithHealth attaches a fresh UHealthComponent; bWithCombat attaches a
	// bare UCombatComponent (the victim-side stun path participant).
	static AActor* M5_013_SpawnBoxActor(UWorld& World, const FVector& Location, bool bWithHealth, bool bWithCombat = false)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_013_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_013_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);

		if (bWithHealth)
		{
			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_013_Health"));
			Health->RegisterComponent();
		}
		if (bWithCombat)
		{
			UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_013_Combat"));
			Combat->RegisterComponent();
		}
		return Actor;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the M1-019 tests use).
	static UAttackCatalog* M5_013_NewCatalog(FAutomationTestBase& Test)
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

	// Independent re-derivation of the design damage formula at the current
	// zero growth attributes: max(1, round(BaseDamage)) == BaseDamage for the
	// four legacy attacks.
	static float M5_013_ExpectedDamage(const UAttackDefinition& Definition)
	{
		const float RawDamage = (Definition.BaseDamage + 0.0f * Definition.AttackCoefficient)
			* 100.0f / (100.0f + FMath::Max(0.0f, 0.0f));
		return FMath::Max(1.0f, FMath::RoundToFloat(RawDamage));
	}

	// Counts HitConfirmed broadcasts and keeps the last payload.
	struct FM5_013_HitEvents
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
	 * One scene: the real catalog, one attacker actor with a combat component
	 * whose feet sit at a fixed injected origin, and helpers to spawn targets
	 * at the active hit box center of a requested attack/facing. The attacker
	 * carries no health component (the M1-019 attacker shape: it is never the
	 * victim of its own attack).
	 */
	struct FM5_013_Scene
	{
		UAttackCatalog* Catalog = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		FM5_013_HitEvents Events;

		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			Catalog = M5_013_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Attacker = M5_013_SpawnBoxActor(World, M5_013_SceneBase, /*bWithHealth*/ false);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M5_013_Combat"));
			if (!Test.TestTrue(TEXT("the attacker combat component attaches the catalog"),
				Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->RegisterComponent();
			Combat->SetFeetLocationProvider([FixedFeet = M5_013_SceneBase]() { return FixedFeet; });
			Events.Bind(*Combat);
			return true;
		}

		const UAttackDefinition* FindDefinition(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			Test.TestTrue(FString::Printf(TEXT("the catalog holds %s"), AttackId), Definition != nullptr);
			return Definition;
		}

		// Spawns a target (health, optionally a bare victim combat component)
		// at the world-space hit box center of this attack/facing pair.
		AActor* SpawnTargetAtBoxCenter(UWorld& World, const UAttackDefinition& Definition, int32 Facing,
			bool bWithCombat = false, const FVector& Offset = FVector::ZeroVector)
		{
			const FVector BoxCenter = ComputeHitBox(M5_013_SceneBase, Facing, Definition).Center;
			return M5_013_SpawnBoxActor(World, BoxCenter + Offset, /*bWithHealth*/ true, bWithCombat);
		}

		static float TargetHealth(AActor& Target)
		{
			const UHealthComponent* Health = Target.FindComponentByClass<UHealthComponent>();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// Advances the component by exactly Count single 1/60 s frames.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Combat->TickCombat(1.0f / 60.0f);
			}
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_013;

// ---------------------------------------------------------------------------
// FourLegacyAttacksDealLegacyDamageThroughUnifiedEntry
// ---------------------------------------------------------------------------

// Each of the four legacy attacks runs its real active-window pipeline against
// a resident target and removes exactly the legacy number (10/14/18/12) once,
// while the component's unified ledger (M5-010) records exactly the one hit
// event of the running instance - the old numbers now flow through the M5-012
// unified entry, not through a second direct-to-health path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_013FourLegacyAttacksDealLegacyDamageThroughUnifiedEntry,
	"UEMMO.Tasks.M5_013.FourLegacyAttacksDealLegacyDamageThroughUnifiedEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_013FourLegacyAttacksDealLegacyDamageThroughUnifiedEntry::RunTest(const FString& Parameters)
{
	struct FLegacyAttackCase
	{
		const TCHAR* AttackId;
		float LegacyDamage;
	};

	// The four legacy numbers of the old pipeline (M5-003 pinned values).
	const FLegacyAttackCase Cases[] = {
		{TEXT("light_01"), 10.0f},
		{TEXT("light_02"), 14.0f},
		{TEXT("launcher"), 18.0f},
		{TEXT("aerial_01"), 12.0f}
	};

	UWorld* World = M5_013_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_013_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_013_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	for (const FLegacyAttackCase& Case : Cases)
	{
		const UAttackDefinition* Definition = Scene.FindDefinition(*this, Case.AttackId);
		if (Definition == nullptr)
		{
			return true;
		}
		if (!TestTrue(FString::Printf(TEXT("%s keeps the legacy base damage"), Case.AttackId),
			FMath::IsNearlyEqual(Definition->BaseDamage, Case.LegacyDamage)))
		{
			return true;
		}

		AActor* Target = Scene.SpawnTargetAtBoxCenter(*World, *Definition, /*Facing*/ 1);
		if (!TestNotNull(FString::Printf(TEXT("%s spawns its target"), Case.AttackId), Target))
		{
			return true;
		}

		// The event count accumulates across the four cases; every case pins
		// its own delta.
		const int32 HitsBeforeCase = Scene.Events.HitCount;

		if (!TestTrue(FString::Printf(TEXT("%s starts"), Case.AttackId),
			Scene.Combat->TryStartAttack(FName(Case.AttackId), /*Facing*/ 1)))
		{
			return true;
		}

		// Tick to the first active frame: the hit lands there exactly once and
		// the unified ledger carries exactly the one live event of this shot.
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(FString::Printf(TEXT("%s hit exactly once on its first active frame"), Case.AttackId),
			Scene.Events.HitCount - HitsBeforeCase, 1);
		TestEqual(FString::Printf(TEXT("%s removed exactly the legacy damage"), Case.AttackId),
			FM5_013_Scene::TargetHealth(*Target), 100.0f - Case.LegacyDamage);
		TestEqual(FString::Printf(TEXT("%s damage equals the design formula result"), Case.AttackId),
			Scene.Events.LastHit.Damage, M5_013_ExpectedDamage(*Definition));
		TestEqual(FString::Printf(TEXT("%s recorded exactly one unified ledger event"), Case.AttackId),
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 1);
		TestEqual(FString::Printf(TEXT("%s accepted exactly one unified ledger control"), Case.AttackId),
			Scene.Combat->GetUnifiedLedgerAcceptedControlCount(), 1);

		// The confirmed payload stays the M1-019 contract: instance id, attack
		// id and the facing-mirrored impulse with the definition launch speed
		// (the launcher's per-cycle policy scale is 1.0 on a fresh cycle).
		TestEqual(FString::Printf(TEXT("%s payload carries the attack id"), Case.AttackId),
			Scene.Events.LastHit.AttackId, FName(Case.AttackId));
		TestTrue(FString::Printf(TEXT("%s payload impulse mirrors the knockback and carries the launch"), Case.AttackId),
			Scene.Events.LastHit.Impulse.Equals(FVector(Definition->KnockbackSpeed, 0.0f, Definition->LaunchSpeed), 1e-4f));

		// The rest of the attack adds no hit; the finished shot releases its
		// ledger keys back to the pool (the next instance can hit again).
		Scene.TickFrames(Definition->DurationFrames + 2);
		TestEqual(FString::Printf(TEXT("%s added no further hit"), Case.AttackId),
			Scene.Events.HitCount - HitsBeforeCase, 1);
		TestEqual(FString::Printf(TEXT("%s target health is unchanged after the attack"), Case.AttackId),
			FM5_013_Scene::TargetHealth(*Target), 100.0f - Case.LegacyDamage);
		TestEqual(FString::Printf(TEXT("%s released its unified ledger events on finish"), Case.AttackId),
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 0);

		// Retire the case target as a corpse: a dead combatant is never a
		// query target again (the read-only query filters dead actors), so
		// the next case's instances can only hit their own resident body.
		// (The temp world has no world context, so DestroyActor is not an
		// option here.)
		if (UHealthComponent* CaseHealth = Target->FindComponentByClass<UHealthComponent>())
		{
			CaseHealth->ApplyDamage(CaseHealth->GetMaxHealth());
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// LegacyRefusalAndInterruptSemanticsHoldThroughUnifiedEntry
// ---------------------------------------------------------------------------

// The M1-019 refusal semantics survive the adaptation: a dead target is
// skipped without recording anything, a get-up protected target (landing
// recovery) refuses the whole hit until the recovery ends, and an interrupted
// instance (cancel mid-window) never hits again while its keys are gone from
// the ledger. A refused hit never poisons a later instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_013LegacyRefusalAndInterruptSemanticsHoldThroughUnifiedEntry,
	"UEMMO.Tasks.M5_013.LegacyRefusalAndInterruptSemanticsHoldThroughUnifiedEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_013LegacyRefusalAndInterruptSemanticsHoldThroughUnifiedEntry::RunTest(const FString& Parameters)
{
	UWorld* World = M5_013_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_013_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_013_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
	if (Definition == nullptr)
	{
		return true;
	}

	// 1. A dead target inside the open window records nothing; the revived
	// target is hit by a fresh instance (nothing was poisoned).
	{
		AActor* Target = Scene.SpawnTargetAtBoxCenter(*World, *Definition, 1);
		if (!TestNotNull(TEXT("the dead-target case spawns its target"), Target))
		{
			return true;
		}
		UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
		TargetHealth->ApplyDamage(TargetHealth->GetMaxHealth());
		if (!TestTrue(TEXT("precondition: the target starts dead"), !TargetHealth->IsAlive()))
		{
			return true;
		}
		if (!TestTrue(TEXT("the dead-target instance starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(TEXT("the open window skips the dead target"), Scene.Events.HitCount, 0);
		TestEqual(TEXT("the dead-target skip recorded no unified ledger event"),
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 0);
		Scene.TickFrames(Definition->DurationFrames + 2);

		TargetHealth->ResetHealth();
		if (!TestTrue(TEXT("precondition: the target is revived at full health"),
			TargetHealth->IsAlive() && FMath::IsNearlyEqual(TargetHealth->GetHealth(), 100.0f)))
		{
			return true;
		}
		if (!TestTrue(TEXT("the post-revive instance starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(TEXT("the fresh instance hits the revived target"), Scene.Events.HitCount, 1);
		TestEqual(TEXT("the revived target lost exactly the legacy damage"),
			FM5_013_Scene::TargetHealth(*Target), 90.0f);
		Scene.TickFrames(Definition->DurationFrames + 2);
		TargetHealth->ApplyDamage(TargetHealth->GetMaxHealth());
	}

	// 2. A get-up protected target (its own landing recovery) refuses the
	// whole hit through the unified entry; after the recovery ends a fresh
	// instance hits normally.
	{
		AActor* Target = Scene.SpawnTargetAtBoxCenter(*World, *Definition, 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the recovery case spawns its target"), Target))
		{
			return true;
		}
		UCombatComponent* VictimCombat = Target->FindComponentByClass<UCombatComponent>();
		if (!TestTrue(TEXT("precondition: the target carries a combat component"), VictimCombat != nullptr))
		{
			return true;
		}
		VictimCombat->SetInputClockSeconds(10.0);
		if (!TestTrue(TEXT("precondition: the target enters its landing recovery"), VictimCombat->BeginLandingRecovery(10.0)))
		{
			return true;
		}
		if (!TestTrue(TEXT("the recovery-case instance starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(TEXT("the open window adds no hit for the get-up protected target"), Scene.Events.HitCount, 1);
		TestEqual(TEXT("the get-up protected target lost no health"),
			FM5_013_Scene::TargetHealth(*Target), 100.0f);
		TestEqual(TEXT("the get-up protected refusal recorded no unified ledger event"),
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 0);
		Scene.TickFrames(Definition->DurationFrames + 2);

		// Walk the victim past both recovery deadlines (0.45 s + 0.25 s).
		VictimCombat->SetInputClockSeconds(10.0 + 0.71);
		VictimCombat->TickCombat(1.0f / 60.0f);
		VictimCombat->TickCombat(1.0f / 60.0f);
		TestTrue(TEXT("precondition: the target recovered to Free"),
			VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);

		if (!TestTrue(TEXT("the post-recovery instance starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(TEXT("the recovered target is hit by the fresh instance"), Scene.Events.HitCount, 2);
		TestEqual(TEXT("the recovered target lost exactly the legacy damage"),
			FM5_013_Scene::TargetHealth(*Target), 90.0f);
		Scene.TickFrames(Definition->DurationFrames + 2);
		if (UHealthComponent* CaseHealth = Target->FindComponentByClass<UHealthComponent>())
		{
			CaseHealth->ApplyDamage(CaseHealth->GetMaxHealth());
		}
	}

	// 3. An interrupted instance never hits again: a mid-window cancel drops
	// the instance's ledger keys with it, and the would-be window frames land
	// nothing afterwards.
	{
		AActor* Target = Scene.SpawnTargetAtBoxCenter(*World, *Definition, 1);
		if (!TestNotNull(TEXT("the interrupt case spawns its target"), Target))
		{
			return true;
		}
		if (!TestTrue(TEXT("the interrupted instance starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(TEXT("precondition: the instance hit once before the cancel"), Scene.Events.HitCount, 3);
		const float HealthBeforeCancel = FM5_013_Scene::TargetHealth(*Target);
		TestEqual(TEXT("the running instance holds one live ledger event before the cancel"),
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 1);

		Scene.Combat->CancelCurrentAttack(FName(TEXT("M5_013_Test")));
		TestEqual(TEXT("the cancel released the instance's ledger keys"),
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 0);

		// The remaining would-be window frames land nothing.
		Scene.TickFrames(Definition->ActiveWindow.EndFrame - Definition->ActiveWindow.StartFrame + 2);
		TestEqual(TEXT("the interrupted instance never hits again"), Scene.Events.HitCount, 3);
		TestEqual(TEXT("the interrupted instance applied no further damage"),
			FM5_013_Scene::TargetHealth(*Target), HealthBeforeCancel);

		// A fresh instance after the interruption hits the same target again.
		if (!TestTrue(TEXT("the post-interrupt instance starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.TickFrames(Definition->ActiveWindow.StartFrame + 1);
		TestEqual(TEXT("the post-interrupt instance hits once"), Scene.Events.HitCount, 4);
		TestEqual(TEXT("the post-interrupt instance deducted the legacy damage"),
			FM5_013_Scene::TargetHealth(*Target), HealthBeforeCancel - 10.0f);
		Scene.TickFrames(Definition->DurationFrames + 2);
		if (UHealthComponent* CaseHealth = Target->FindComponentByClass<UHealthComponent>())
		{
			CaseHealth->ApplyDamage(CaseHealth->GetMaxHealth());
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// MeleeEnemyAttacksFlowThroughUnifiedEntry
// ---------------------------------------------------------------------------

// The production melee enemy uses the same single pipeline: a real
// AMeleeEnemy attacker drives its own combat component through light_01, its
// unified ledger records the hit event, and the M2-004 same-kind exclusion
// still refuses a target of its own kind while a foreign target takes the
// legacy damage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_013MeleeEnemyAttacksFlowThroughUnifiedEntry,
	"UEMMO.Tasks.M5_013.MeleeEnemyAttacksFlowThroughUnifiedEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_013MeleeEnemyAttacksFlowThroughUnifiedEntry::RunTest(const FString& Parameters)
{
	UWorld* World = M5_013_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_013_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_013_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
	if (Definition == nullptr)
	{
		return true;
	}

	// The attacker is a real melee enemy; its combat component gets the real
	// catalog and a pinned feet origin (deterministic hit box placement).
	AMeleeEnemy* EnemyAttacker = World->SpawnActor<AMeleeEnemy>(AMeleeEnemy::StaticClass(),
		M5_013_SceneBase, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the melee enemy attacker spawns"), EnemyAttacker))
	{
		return true;
	}
	UCombatComponent* EnemyCombat = EnemyAttacker->GetCombatComponent();
	if (!TestTrue(TEXT("the melee enemy carries a combat component with the catalog"),
		EnemyCombat != nullptr && EnemyCombat->InitializeFromCatalog(Scene.Catalog)))
	{
		return true;
	}
	EnemyCombat->SetFeetLocationProvider([FixedFeet = M5_013_SceneBase]() { return FixedFeet; });

	int32 EnemyHitCount = 0;
	FCombatHit EnemyLastHit;
	EnemyCombat->OnHitConfirmed.AddLambda([&](const FCombatHit& Hit)
	{
		++EnemyHitCount;
		EnemyLastHit = Hit;
	});

	// One same-kind target (another melee enemy) at the box center and one
	// foreign box target inside the same box depth.
	const FVector BoxCenter = ComputeHitBox(M5_013_SceneBase, 1, *Definition).Center;
	AMeleeEnemy* EnemyTarget = World->SpawnActor<AMeleeEnemy>(AMeleeEnemy::StaticClass(),
		BoxCenter, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the same-kind enemy target spawns"), EnemyTarget))
	{
		return true;
	}
	AActor* ForeignTarget = Scene.SpawnTargetAtBoxCenter(*World, *Definition, 1, /*bWithCombat*/ false,
		FVector(0.0, 40.0, 0.0));
	if (!TestNotNull(TEXT("the foreign target spawns"), ForeignTarget))
	{
		return true;
	}

	if (!TestTrue(TEXT("the melee enemy starts light_01"), EnemyCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}

	const int32 WindowFrames = Definition->ActiveWindow.StartFrame + 1;
	for (int32 Index = 0; Index < WindowFrames; ++Index)
	{
		EnemyCombat->TickCombat(1.0f / 60.0f);
	}

	// Exactly one confirmed hit - the foreign target's - with the legacy
	// number, and exactly one unified ledger event on the enemy's own
	// component: the production enemy path is the unified entry.
	TestEqual(TEXT("the melee enemy attack confirmed exactly one hit"), EnemyHitCount, 1);
	TestTrue(TEXT("the confirmed hit names the foreign target"), EnemyLastHit.Target.Get() == ForeignTarget);
	TestEqual(TEXT("the foreign target lost exactly the legacy damage"),
		FM5_013_Scene::TargetHealth(*ForeignTarget), 90.0f);
	TestEqual(TEXT("the same-kind enemy target lost no health"),
		FM5_013_Scene::TargetHealth(*EnemyTarget), 100.0f);
	TestEqual(TEXT("the melee enemy's unified ledger recorded exactly one event"),
		EnemyCombat->GetUnifiedLedgerRecordedEventCount(), 1);
	TestEqual(TEXT("the melee enemy's unified ledger accepted exactly one control"),
		EnemyCombat->GetUnifiedLedgerAcceptedControlCount(), 1);

	// The rest of the attack adds nothing.
	for (int32 Index = 0; Index < Definition->DurationFrames + 2; ++Index)
	{
		EnemyCombat->TickCombat(1.0f / 60.0f);
	}
	TestEqual(TEXT("the melee enemy attack added no further hit"), EnemyHitCount, 1);
	return true;
}

#endif
