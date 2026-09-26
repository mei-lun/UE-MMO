#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/HealthComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_019
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is used as the feet origin of
	// the default scene (the attacker actor root sits at its feet).
	const FVector M1_019_SceneBase(40000.0, 45000.0, 600.0);

	// Half extent of one test body box; small enough to sit fully inside the
	// light_01 hit box (half extent (85,50,70) from the attack definition).
	const FVector M1_019_BodyHalfExtent(20.0, 20.0, 30.0);

	// World acquisition, same order as the M1-018 pattern: a private temp
	// world first, the shared game world as fallback. The choice is logged so
	// the task report can quote it.
	static UWorld* M1_019_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_019_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_019 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_019 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Spawns a plain actor with one query-enabled box root at Location.
	// bWithHealth attaches a fresh UHealthComponent; bStartDead kills it
	// through the only damage entry point (ApplyDamage) before use.
	static AActor* M1_019_SpawnBoxActor(UWorld& World, const FVector& Location, bool bWithHealth, bool bStartDead = false)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_019_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_019_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);

		if (bWithHealth)
		{
			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M1_019_Health"));
			Health->RegisterComponent();
			if (bStartDead)
			{
				Health->ApplyDamage(Health->GetMaxHealth());
			}
		}
		return Actor;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the M1-011 lifecycle tests use). Returns
	// null after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_019_NewCatalog(FAutomationTestBase& Test)
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

	// Independent re-derivation of the design damage formula (Docs/01 section
	// 8.2) with the current zero growth attributes: no attacker AttackPower
	// and no defender Defense exist yet, so the result is max(1, round(
	// BaseDamage)) and light_01 stays verifiable at 10.
	static float M1_019_ExpectedDamage(const UAttackDefinition& Definition)
	{
		const float AttackPower = 0.0f;
		const float Defense = 0.0f;
		const float RawDamage = (Definition.BaseDamage + AttackPower * Definition.AttackCoefficient)
			* 100.0f / (100.0f + FMath::Max(0.0f, Defense));
		return FMath::Max(1.0f, FMath::RoundToFloat(RawDamage));
	}

	// Counts HitConfirmed broadcasts and keeps the last payload.
	struct FM1_019_HitEvents
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

	// One scene: the real catalog, an attacker actor with a combat component
	// whose feet sit at FeetLocation, and one alive target inside the
	// light_01 hit box for the requested facing.
	struct FM1_019_Scene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Definition = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		AActor* Target = nullptr;
		FVector BoxCenter = FVector::ZeroVector;
		FM1_019_HitEvents Events;

		// Builds attacker + target and asserts the light_01 source values the
		// acceptance criteria are phrased against. Returns false after
		// reporting the problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& FeetLocation, int32 Facing, bool bInjectFeetProvider)
		{
			Catalog = M1_019_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Definition = Catalog->Find(FName(TEXT("light_01")));
			if (!Test.TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("light_01 keeps the design initial values (duration 26, window [7,11), damage 10, knockback 90, hit stop 0.04)"),
				Definition->DurationFrames == 26
				&& Definition->ActiveWindow.StartFrame == 7 && Definition->ActiveWindow.EndFrame == 11
				&& FMath::IsNearlyEqual(Definition->BaseDamage, 10.0f)
				&& FMath::IsNearlyEqual(Definition->KnockbackSpeed, 90.0f)
				&& FMath::IsNearlyEqual(Definition->HitStopSeconds, 0.04f)))
			{
				return false;
			}

			Attacker = M1_019_SpawnBoxActor(World, FeetLocation, /*bWithHealth*/ false);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M1_019_Combat"));
			if (!Test.TestTrue(TEXT("the attacker combat component attaches the catalog"),
				Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->RegisterComponent();
			if (bInjectFeetProvider)
			{
				Combat->SetFeetLocationProvider([FixedFeet = FeetLocation]() { return FixedFeet; });
			}
			Events.Bind(*Combat);

			// The target sits at the world-space hit box center for this
			// facing, so its small body is fully inside the attack's box.
			BoxCenter = ComputeHitBox(FeetLocation, Facing, *Definition).Center;
			Target = M1_019_SpawnBoxActor(World, BoxCenter, /*bWithHealth*/ true);
			if (!Test.TestNotNull(TEXT("the target spawns"), Target))
			{
				return false;
			}
			const UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
			return Test.TestTrue(TEXT("the target starts alive at full health"),
				TargetHealth != nullptr && TargetHealth->IsAlive() && FMath::IsNearlyEqual(TargetHealth->GetHealth(), 100.0f));
		}

		// Starts light_01 with the requested facing (must succeed).
		bool Start(FAutomationTestBase& Test, int32 Facing)
		{
			return Test.TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), Facing));
		}

		// Advances the component by exactly Count single 1/60 s frames.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Combat->TickCombat(1.0f / 60.0f);
			}
		}

		float TargetHealth() const
		{
			const UHealthComponent* Health = Target->FindComponentByClass<UHealthComponent>();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_019;

// The core acceptance case: light_01 ticks through its multi-frame active
// window [7,11) against one resident target and removes exactly 10 HP once
// (100 -> 90), no matter how many active frames follow. The remaining active
// frames and the rest of the attack never deduct again (the HP does not sink
// towards 60), and exactly one HitConfirmed broadcast carries InstanceId 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019ActiveWindowMultiStepHitsExactlyOnce,
	"UEMMO.Tasks.M1_019.ActiveWindowMultiStepHitsExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019ActiveWindowMultiStepHitsExactlyOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, M1_019_SceneBase, /*Facing*/ 1, /*bInjectFeetProvider*/ false))
	{
		return true;
	}

	if (!Scene.Start(*this, 1))
	{
		return true;
	}

	// Frames 0..6: before the active window opens, nothing happens.
	Scene.TickFrames(7);
	TestEqual(TEXT("frames 0..6 produce no hit"), Scene.Events.HitCount, 0);
	TestEqual(TEXT("frames 0..6 leave the target at full health"), Scene.TargetHealth(), 100.0f);

	// Frames 7..10: the active window. The hit lands on frame 7 and the
	// following active frames are deduplicated by the instance hit set.
	Scene.TickFrames(4);
	TestEqual(TEXT("the active window hits exactly once"), Scene.Events.HitCount, 1);
	TestEqual(TEXT("one accepted hit removed exactly the base damage"), Scene.TargetHealth(), 90.0f);
	TestEqual(TEXT("the broadcast carries the running instance id"), Scene.Events.LastHit.AttackInstanceId, uint64(1));

	// Frames 11..25: the rest of the attack. No second hit may appear.
	Scene.TickFrames(15);
	TestEqual(TEXT("the attack end adds no further hit"), Scene.Events.HitCount, 1);
	TestEqual(TEXT("the target HP stays at 90 and never sinks towards 60"), Scene.TargetHealth(), 90.0f);
	TestTrue(TEXT("the attack finished back to Free"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);

	// Facing +1 mirrors the impulse X to the positive direction.
	TestTrue(TEXT("the hit impulse X carries the positive knockback speed and Z the launch speed"),
		Scene.Events.LastHit.Impulse.Equals(FVector(Scene.Definition->KnockbackSpeed, 0.0f, Scene.Definition->LaunchSpeed), 1e-4f));
	return true;
}

// A finished instance, a fresh start after the natural end and a fresh start
// after a mid-instance reset can each deduct again: the instance hit set dies
// with the instance, so a new AttackInstanceId hits the same target once more.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019NewInstanceHitsAgainAfterFinishAndAfterReset,
	"UEMMO.Tasks.M1_019.NewInstanceHitsAgainAfterFinishAndAfterReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019NewInstanceHitsAgainAfterFinishAndAfterReset::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, M1_019_SceneBase, /*Facing*/ 1, /*bInjectFeetProvider*/ false))
	{
		return true;
	}

	// Instance 1 plays out fully and hits once.
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	Scene.TickFrames(26);
	TestEqual(TEXT("instance 1 hit exactly once"), Scene.Events.HitCount, 1);
	TestEqual(TEXT("instance 1 left the target at 90"), Scene.TargetHealth(), 90.0f);

	// A new instance after the natural end may deduct again.
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the restart minted a fresh instance id"), Scene.Combat->GetSnapshot().InstanceId, uint64(2));
	Scene.TickFrames(8);
	TestEqual(TEXT("instance 2 hit exactly once"), Scene.Events.HitCount, 2);
	TestEqual(TEXT("instance 2 deducted 10 more (90 -> 80)"), Scene.TargetHealth(), 80.0f);
	TestEqual(TEXT("the new hit carries the new instance id"), Scene.Events.LastHit.AttackInstanceId, uint64(2));

	// Let instance 2 finish before opening the reset part of the test.
	Scene.TickFrames(18);
	TestTrue(TEXT("instance 2 finished back to Free"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);

	// A new instance after a mid-instance reset may deduct again.
	UHealthComponent* TargetHealth = Scene.Target->FindComponentByClass<UHealthComponent>();
	TargetHealth->ResetHealth();
	TestEqual(TEXT("the target is back at full health"), Scene.TargetHealth(), 100.0f);
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	Scene.TickFrames(8);
	TestEqual(TEXT("instance 3 hit exactly once"), Scene.Events.HitCount, 3);
	TestEqual(TEXT("instance 3 deducted 10 (100 -> 90)"), Scene.TargetHealth(), 90.0f);
	TestEqual(TEXT("the third hit carries instance id 3"), Scene.Events.LastHit.AttackInstanceId, uint64(3));

	Scene.Combat->ResetCombat();
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the post-reset start minted instance id 4"), Scene.Combat->GetSnapshot().InstanceId, uint64(4));
	Scene.TickFrames(8);
	TestEqual(TEXT("instance 4 hit exactly once"), Scene.Events.HitCount, 4);
	TestEqual(TEXT("instance 4 deducted 10 more (90 -> 80)"), Scene.TargetHealth(), 80.0f);
	TestEqual(TEXT("the fourth hit carries instance id 4"), Scene.Events.LastHit.AttackInstanceId, uint64(4));
	return true;
}

// Frames outside [7,11) never deduct: before the window, during the window
// against a dead target (no key is recorded for a refused hit) and after the
// window. A fresh instance at the end still hits, proving the skipped dead
// target poisoned nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019OutsideWindowAndDeadTargetNeverHit,
	"UEMMO.Tasks.M1_019.OutsideWindowAndDeadTargetNeverHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019OutsideWindowAndDeadTargetNeverHit::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, M1_019_SceneBase, /*Facing*/ 1, /*bInjectFeetProvider*/ false))
	{
		return true;
	}

	if (!Scene.Start(*this, 1))
	{
		return true;
	}

	// Frames 0..6: before the window with a live target.
	Scene.TickFrames(7);
	TestEqual(TEXT("no hit before the window opens"), Scene.Events.HitCount, 0);
	TestEqual(TEXT("the target keeps full health before the window"), Scene.TargetHealth(), 100.0f);

	// Frames 7..10: the window is open but the target is dead.
	UHealthComponent* TargetHealth = Scene.Target->FindComponentByClass<UHealthComponent>();
	TargetHealth->ApplyDamage(TargetHealth->GetMaxHealth());
	TestTrue(TEXT("precondition: the target is dead"), !TargetHealth->IsAlive());
	Scene.TickFrames(4);
	TestEqual(TEXT("the open window skips the dead target"), Scene.Events.HitCount, 0);

	// Frames 11..25: the target is alive again but the window already passed.
	TargetHealth->ResetHealth();
	TestTrue(TEXT("precondition: the target is alive again at full health"),
		TargetHealth->IsAlive() && FMath::IsNearlyEqual(TargetHealth->GetHealth(), 100.0f));
	Scene.TickFrames(15);
	TestEqual(TEXT("no hit after the window passed"), Scene.Events.HitCount, 0);
	TestEqual(TEXT("the target still has full health after the whole attack"), Scene.TargetHealth(), 100.0f);

	// Positive control: a fresh instance against the same target hits once,
	// so the skipped dead frames recorded no dedup key.
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	Scene.TickFrames(8);
	TestEqual(TEXT("a fresh instance hits the revived target exactly once"), Scene.Events.HitCount, 1);
	TestEqual(TEXT("the fresh instance deducted 10 (100 -> 90)"), Scene.TargetHealth(), 90.0f);
	return true;
}

// A frozen clock drops frames without hits; after unfreezing, one long frame
// advances up to 8 logic steps straight across the window [7,11) and the hit
// still lands exactly once (no miss, no duplicate).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019LongFrameCrossingWindowHitsExactlyOnce,
	"UEMMO.Tasks.M1_019.LongFrameCrossingWindowHitsExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019LongFrameCrossingWindowHitsExactlyOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, M1_019_SceneBase, /*Facing*/ 1, /*bInjectFeetProvider*/ false))
	{
		return true;
	}

	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	// Frames 0..6: stop right in front of the window.
	Scene.TickFrames(7);
	TestEqual(TEXT("no hit before the window"), Scene.Events.HitCount, 0);

	// Frozen frames drop their deltas and advance nothing.
	Scene.Combat->SetClockFrozen(true);
	Scene.TickFrames(5);
	TestEqual(TEXT("frozen frames produce no hit"), Scene.Events.HitCount, 0);
	TestEqual(TEXT("frozen frames advance no frame"), Scene.Combat->GetSnapshot().Frame, 6);
	Scene.Combat->SetClockFrozen(false);

	// One long frame: 8/60 s advances 8 logic steps (7..14) straight across
	// the whole active window; the hit lands once and only once.
	Scene.Combat->TickCombat(8.0f / 60.0f);
	TestEqual(TEXT("the long frame crossing the window hits exactly once"), Scene.Events.HitCount, 1);
	TestEqual(TEXT("the long frame removed exactly the base damage"), Scene.TargetHealth(), 90.0f);
	TestEqual(TEXT("the long frame landed on frame 14"), Scene.Combat->GetSnapshot().Frame, 14);
	TestTrue(TEXT("the attack is still running after the long frame"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Attacking);

	// The remaining frames add nothing.
	Scene.TickFrames(20);
	TestEqual(TEXT("the rest of the attack adds no hit"), Scene.Events.HitCount, 1);
	TestEqual(TEXT("the target HP stays at 90"), Scene.TargetHealth(), 90.0f);
	return true;
}

// Dead targets and destroyed actors are safe: a target killed before the
// start and an actor destroyed while its weak reference lingers both pass the
// whole attack without a crash, a hit or any state damage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019DeadAndDestroyedActorsAreSafeSkips,
	"UEMMO.Tasks.M1_019.DeadAndDestroyedActorsAreSafeSkips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019DeadAndDestroyedActorsAreSafeSkips::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, M1_019_SceneBase, /*Facing*/ 1, /*bInjectFeetProvider*/ false))
	{
		return true;
	}

	// A dead target from the very start never becomes a hit.
	UHealthComponent* TargetHealth = Scene.Target->FindComponentByClass<UHealthComponent>();
	TargetHealth->ApplyDamage(TargetHealth->GetMaxHealth());
	TestTrue(TEXT("precondition: the target starts dead"), !TargetHealth->IsAlive());
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	Scene.TickFrames(26);
	TestEqual(TEXT("the full attack against a dead target hits nothing"), Scene.Events.HitCount, 0);
	TestTrue(TEXT("the attack still completed normally"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);

	// A destroyed actor with a lingering weak reference must not crash the
	// next attack either. It is killed first so even a failed destroy keeps
	// the flow deterministic; the query also filters dead actors upstream.
	TWeakObjectPtr<AActor> LingeringReference = Scene.Target;
	TargetHealth->ApplyDamage(TargetHealth->GetMaxHealth());
	Scene.Target->Destroy();
	TestTrue(TEXT("precondition: the destroyed actor leaves a stale weak reference"), !LingeringReference.IsValid());
	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	Scene.TickFrames(26);
	TestEqual(TEXT("the full attack against a destroyed actor hits nothing"), Scene.Events.HitCount, 0);
	TestTrue(TEXT("the attack after a destroyed target still completed"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// Two resident targets inside the same box are both hit on the very first
// active frame, each exactly once, and the rest of the window adds nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019MultipleTargetsAllHitOnceSameFrame,
	"UEMMO.Tasks.M1_019.MultipleTargetsAllHitOnceSameFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019MultipleTargetsAllHitOnceSameFrame::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, M1_019_SceneBase, /*Facing*/ 1, /*bInjectFeetProvider*/ false))
	{
		return true;
	}
	// A second target beside the first one, both inside the box depth.
	AActor* SecondTarget = M1_019_SpawnBoxActor(*World, Scene.BoxCenter + FVector(0.0, 40.0, 0.0), /*bWithHealth*/ true);
	if (!TestNotNull(TEXT("the second target spawns"), SecondTarget))
	{
		return true;
	}

	if (!Scene.Start(*this, 1))
	{
		return true;
	}
	// Frames 0..7: the first active frame is frame 7.
	Scene.TickFrames(8);
	TestEqual(TEXT("both targets were hit on the first active frame"), Scene.Events.HitCount, 2);
	TestEqual(TEXT("the first target lost exactly the base damage"), Scene.TargetHealth(), 90.0f);
	const UHealthComponent* SecondHealth = SecondTarget->FindComponentByClass<UHealthComponent>();
	TestEqual(TEXT("the second target lost exactly the base damage"), SecondHealth->GetHealth(), 90.0f);

	// The remaining frames deduct neither target again.
	Scene.TickFrames(18);
	TestEqual(TEXT("the rest of the window adds no hit"), Scene.Events.HitCount, 2);
	TestEqual(TEXT("the first target stays at 90"), Scene.TargetHealth(), 90.0f);
	TestEqual(TEXT("the second target stays at 90"), SecondHealth->GetHealth(), 90.0f);
	return true;
}

// The HitConfirmed payload carries every contract field: instigator reference
// and stable id, instance id, hit group, target, the actual damage, the
// facing-mirrored impulse, the world hit location, the attack id and the hit
// stop duration. This scene injects an explicit feet origin and attacks with
// facing -1, so the mirrored values are checked against the negative side.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_019HitConfirmedPayloadCarriesContractFields,
	"UEMMO.Tasks.M1_019.HitConfirmedPayloadCarriesContractFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_019HitConfirmedPayloadCarriesContractFields::RunTest(const FString& Parameters)
{
	UWorld* World = M1_019_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	// The injected feet origin sits away from the attacker actor location, so
	// the payload can only match if the provider was actually used.
	const FVector InjectedFeet = M1_019_SceneBase + FVector(0.0, 600.0, 0.0);
	FM1_019_Scene Scene;
	if (!Scene.Build(*this, *World, InjectedFeet, /*Facing*/ -1, /*bInjectFeetProvider*/ true))
	{
		return true;
	}

	if (!Scene.Start(*this, -1))
	{
		return true;
	}
	Scene.TickFrames(8);
	TestEqual(TEXT("the injected-feet scene hit exactly once"), Scene.Events.HitCount, 1);
	if (Scene.Events.HitCount != 1)
	{
		return true;
	}

	const FCombatHit Hit = Scene.Events.LastHit;
	TestTrue(TEXT("the payload carries the attacker actor"), Hit.Instigator.Get() == Scene.Attacker);
	TestTrue(TEXT("the payload carries the component's stable instigator id"),
		Hit.InstigatorId == Scene.Combat->GetInstigatorId() && Hit.InstigatorId != 0);
	TestEqual(TEXT("the payload carries the attack instance id"), Hit.AttackInstanceId, uint64(1));
	TestEqual(TEXT("the payload carries the single default hit group"), Hit.HitGroupId, 0);
	TestTrue(TEXT("the payload carries the damaged target actor"), Hit.Target.Get() == Scene.Target);
	TestEqual(TEXT("the payload carries the actual damage from the design formula"),
		Hit.Damage, M1_019_ExpectedDamage(*Scene.Definition));
	TestEqual(TEXT("the current zero-attribute damage equals the light_01 base damage"), Hit.Damage, 10.0f);
	TestTrue(TEXT("the payload carries the hit box center as world hit location"),
		Hit.WorldHitLocation.Equals(Scene.BoxCenter, 1e-4f) && !Hit.WorldHitLocation.IsZero());
	TestEqual(TEXT("the payload carries the attack id"), Hit.AttackId, FName(TEXT("light_01")));

	// Facing -1 mirrors the knockback to the negative X side; Y stays 0 and Z
	// carries the launch speed. The physical impulse application itself skips
	// the query-only test body (no simulation), which this card documents.
	const FVector ExpectedImpulse(-Scene.Definition->KnockbackSpeed, 0.0f, Scene.Definition->LaunchSpeed);
	TestTrue(TEXT("the payload impulse mirrors the knockback by the facing"),
		Hit.Impulse.Equals(ExpectedImpulse, 1e-4f));
	TestTrue(TEXT("the payload carries the definition's hit stop duration"),
		FMath::IsNearlyEqual(Hit.HitStopSeconds, Scene.Definition->HitStopSeconds) && Hit.HitStopSeconds > 0.0f);

	// The accepted damage really removed health from the target.
	TestEqual(TEXT("the target lost exactly the reported damage"), Scene.TargetHealth(), 100.0f - Hit.Damage);
	return true;
}

#endif
