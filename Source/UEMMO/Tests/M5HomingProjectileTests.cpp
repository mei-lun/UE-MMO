// M5-030: the homing motion policy tests. Pins the registry-id target
// binding (no raw world address, no friendly, no unregistered/dangling
// target, no invented target), the bounded-turn guidance (per-step turn
// never exceeds the configured rate, speed stays exactly the launch speed,
// the curvature follows the configured rate), the pursuit of a moving
// target, both lost-target actions (unregister -> keep straight, destroy ->
// terminate), the never-re-search face, and the shared hit semantics (one
// unified submission, continuous collision, no residual callbacks, lifetime
// untouched).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/LinearProjectilePolicy.h"
#include "../Projectiles/Policies/HomingPolicy.h"

#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_030
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the flight axis, Y the depth axis, Z height.
	const FVector M5_030_SceneBase(120000.0, 70000.0, 800.0);

	// Flight speed for every case: 6000 cm/s (a 1/60s step covers 100cm).
	const float M5_030_SpeedCmS = 6000.0f;

	/** A fully resolved per-hit profile (pure value; the policy never reads catalogs). */
	static FDamageProfile M5_030_MakeProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("homing_test_round");
		Profile.BaseDamage = 7.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 1.0f;
		Profile.KnockbackCmPerSecond = 70.0f;
		Profile.LaunchCmPerSecond = 700.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** Test seam for the hit-to-entity face: the scene registers its id map here. */
	struct FTestTargetIdentity : IHitscanTargetIdentity
	{
		TMap<const AActor*, FEntityId> IdsByActor;

		void Record(const AActor& Actor, FEntityId Id)
		{
			IdsByActor.Add(&Actor, Id);
		}

		virtual FEntityId ResolveTargetEntityId(const AActor& HitActor) const override
		{
			const FEntityId* Id = IdsByActor.Find(&HitActor);
			return Id != nullptr ? *Id : InvalidCombatTargetId;
		}
	};

	/** World acquisition, the M5_012 pattern: private temp world, GWorld fallback. */
	static UWorld* M5_030_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_030_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_030 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_030 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_030_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_030_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	/**
	 * One scene: registry + bound ledger + identity seam and the registered
	 * source entity. The shooter is a body-less registered actor; targets are
	 * spawned per case on their own depth lane.
	 */
	struct FM5_030_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FTestTargetIdentity Identity;
		FCombatEpoch Epoch = 1;
		FEntityId ShooterId = InvalidCombatEntityId;
		AActor* Shooter = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& ShooterLocation)
		{
			FActorSpawnParameters Params;
			Shooter = World.SpawnActor<AActor>(AActor::StaticClass(), ShooterLocation, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("a shooter actor spawned"), Shooter))
			{
				return false;
			}
			FCombatEntityMetadata Metadata;
			Metadata.Faction = TEXT("Player");
			Metadata.Category = TEXT("shooter");
			ShooterId = Registry.RegisterEntity(Shooter, Metadata, Epoch);
			if (!Test.TestTrue(TEXT("the shooter registered with a valid id"), ShooterId != InvalidCombatEntityId))
			{
				return false;
			}
			Identity.Record(*Shooter, ShooterId);
			return true;
		}

		/** The minted id of a spawned/registered actor through the identity seam. */
		FEntityId IdOf(const AActor& Actor) const
		{
			const FEntityId* Id = Identity.IdsByActor.Find(&Actor);
			return Id != nullptr ? *Id : InvalidCombatEntityId;
		}
	};

	/** Spawns a query-only box body (target body) at Center. */
	static AActor* M5_030_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& Extent)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_030_Box"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(Extent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		// The projectile body blocks this object type; the blocking face needs
		// the reciprocal response (either side ignoring kills the block pair).
		Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Body->RegisterComponent();
		Body->SetWorldLocation(Center);
		return Actor;
	}

	/** Spawns a registered hostile/friendly target (box body + health + combat). */
	static AActor* M5_030_SpawnRegisteredTarget(FAutomationTestBase& Test, UWorld& World, FM5_030_Scene& Scene,
		const FVector& Center, const TCHAR* Faction)
	{
		AActor* Actor = M5_030_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0));
		if (!Test.TestNotNull(TEXT("a target actor spawned"), Actor))
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_030_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_030_Combat"));
		Combat->RegisterComponent();
		FCombatEntityMetadata Metadata;
		Metadata.Faction = Faction;
		Metadata.Category = TEXT("target");
		const FEntityId Id = Scene.Registry.RegisterEntity(Actor, Metadata, Scene.Epoch);
		if (!Test.TestTrue(TEXT("the target registered with a valid id"), Id != InvalidCombatEntityId))
		{
			return nullptr;
		}
		Scene.Identity.Record(*Actor, Id);
		return Actor;
	}

	/** Spawns a pellet actor with its provenance stamped (homing carries gravity scale 0). */
	static ACombatProjectile* M5_030_SpawnProjectile(FAutomationTestBase& Test, UWorld& World,
		const FVector& Origin, FEntityId SourceId)
	{
		FActorSpawnParameters Params;
		ACombatProjectile* Actor = World.SpawnActor<ACombatProjectile>(ACombatProjectile::StaticClass(),
			Origin, FRotator::ZeroRotator, Params);
		if (!Test.TestNotNull(TEXT("a projectile actor spawned"), Actor))
		{
			return nullptr;
		}
		Actor->Context.Epoch = 1;
		Actor->Context.SourceEntityId = SourceId;
		Actor->Context.ShotId = 600;
		Actor->Context.PelletIndex = 0;
		Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
		Actor->Context.ProjectileId = TEXT("missile_homing_test");
		Actor->LaunchSpeedCmS = M5_030_SpeedCmS;
		Actor->MotionGravityScale = 0.0f;
		return Actor;
	}

	/** The default hit context for one scene (all filter bits on, no pierce). */
	static FProjectileHitContext M5_030_MakeHitContext(FM5_030_Scene& Scene, int32 PierceCount = 0)
	{
		FProjectileHitContext Context;
		Context.Registry = &Scene.Registry;
		Context.Ledger = &Scene.Ledger;
		Context.Identity = &Scene.Identity;
		Context.AttackProfile = M5_030_MakeProfile();
		Context.PierceCount = PierceCount;
		Context.bFriendliesBlockShot = true;
		Context.bUnregisteredActorsBlockShot = true;
		Context.SourceActor = Scene.Shooter;
		return Context;
	}

	/** Configures, binds and begins one homing pellet; a refusal fails the case. */
	static bool M5_030_Begin(FAutomationTestBase& Test, FHomingProjectilePolicy& Policy, ACombatProjectile& Actor,
		const FVector& Direction, const FProjectileHitContext& Context, FEntityId TargetId, float TurnRateDegS,
		EHomingLostTargetAction LostAction, const TCHAR* What)
	{
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the target id bound"), What), Policy.BindTarget(TargetId)))
		{
			return false;
		}
		Policy.Configure(TurnRateDegS, LostAction);
		return Test.TestTrue(FString::Printf(TEXT("%s: the policy began"), What), Policy.Begin(Actor, Direction, Context));
	}

	/** The unit direction of one per-step displacement. */
	static FVector M5_030_StepDirection(const FVector& From, const FVector& To)
	{
		return (To - From).GetSafeNormal();
	}
}

using namespace UE::UEMMO::Tasks::M5_030;

// ---------------------------------------------------------------------------
// PursuitCurvatureBoundedAndSpeedConstant
// ---------------------------------------------------------------------------

// The guidance face on a static target, two turn rates on two depth lanes:
// every per-step direction change stays within the configured turn rate (the
// bounded steering acceleration |a| = v * omega), the speed stays exactly the
// launch speed forever, and the higher rate curves onto the target strictly
// sooner (the curvature follows the configuration).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_030PursuitCurvatureBoundedAndSpeedConstant,
	"UEMMO.Tasks.M5_030.PursuitCurvatureBoundedAndSpeedConstant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_030PursuitCurvatureBoundedAndSpeedConstant::RunTest(const FString& Parameters)
{
	UWorld* World = M5_030_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_030_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_030_Scene Scene;
	if (!Scene.Build(*this, *World, M5_030_SceneBase + FVector(-100.0, -2000.0, 0.0)))
	{
		return true;
	}

	// Two lanes (Y depth axis), two turn rates; each target sits 6000cm ahead
	// and 2000cm off the lane (an ~18 degrees initial bearing off the +X
	// launch direction). The geometry keeps the guided intercept well inside
	// the budget for BOTH rates: a too-short lane with a wide bearing would
	// let the slow-rate pellet overshoot its still-uncompleted turn (its
	// turn-circle radius v/omega is 3820cm at 90 deg/s) and circle instead of
	// converging - the first run recorded exactly that (the fixture defect,
	// fixed by the longer lane, assertions unchanged in meaning). One world,
	// no shared geometry between the lanes.
	const float TurnRates[2] = { 90.0f, 240.0f };
	ACombatProjectile* Pellets[2] = { nullptr, nullptr };
	AActor* Targets[2] = { nullptr, nullptr };
	UHealthComponent* TargetHealths[2] = { nullptr, nullptr };
	FHomingProjectilePolicy Policies[2];
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const float LaneY = 3000.0f * Index;
		Targets[Index] = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
			M5_030_SceneBase + FVector(6000.0, LaneY + 2000.0, 0.0), TEXT("Enemy"));
		if (Targets[Index] == nullptr)
		{
			return true;
		}
		TargetHealths[Index] = Targets[Index]->FindComponentByClass<UHealthComponent>();
		Pellets[Index] = M5_030_SpawnProjectile(*this, *World, M5_030_SceneBase + FVector(0.0, LaneY, 0.0), Scene.ShooterId);
		if (Pellets[Index] == nullptr || TargetHealths[Index] == nullptr)
		{
			return true;
		}
		if (!M5_030_Begin(*this, Policies[Index], *Pellets[Index], FVector(1.0, 0.0, 0.0),
			M5_030_MakeHitContext(Scene), Scene.IdOf(*Targets[Index]), TurnRates[Index],
			EHomingLostTargetAction::KeepStraight, *FString::Printf(TEXT("the turn-%.0f pellet"), TurnRates[Index])))
		{
			return true;
		}
	}

	// Advance both pellets in lockstep until each terminates; per alive step,
	// the speed is pinned to the launch speed exactly and the per-step
	// direction change (the displacement bearing change) stays within the
	// configured turn rate.
	const float MaxTurnDegPerStep[2] = { TurnRates[0] / 60.0f, TurnRates[1] / 60.0f };
	int32 FinishSteps[2] = { 0, 0 };
	FVector PrevLocations[2] = { Pellets[0]->GetActorLocation(), Pellets[1]->GetActorLocation() };
	FVector PrevStepDirections[2] = { FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0) };
	float BearingAfterStep10[2] = { -1.0f, -1.0f };
	for (int32 Step = 1; Step <= 120; ++Step)
	{
		bool bAnyAlive = false;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			if (Policies[Index].IsFinished())
			{
				continue;
			}
			bAnyAlive = true;
			Policies[Index].AdvanceMotion(1.0f / 60.0f);
			const FVector NewLocation = Pellets[Index]->GetActorLocation();
			const FVector StepDirection = M5_030_StepDirection(PrevLocations[Index], NewLocation);
			PrevLocations[Index] = NewLocation;
			FinishSteps[Index] = Step;
			if (Step == 10)
			{
				// The instantaneous bearing to the target: the higher rate
				// must have curved closer onto it (the curvature follows the
				// configuration).
				const FVector ToTarget = (Targets[Index]->GetActorLocation() - NewLocation).GetSafeNormal();
				BearingAfterStep10[Index] = FMath::RadiansToDegrees(FMath::Acos(
					FMath::Clamp(FVector::DotProduct(StepDirection, ToTarget), -1.0f, 1.0f)));
			}
			if (Policies[Index].IsFinished())
			{
				continue; // the terminating step carried the hit; no alive face left to pin
			}
			const FVector Velocity = Pellets[Index]->Movement->Velocity;
			TestTrue(FString::Printf(TEXT("turn-%.0f step %d keeps the launch speed exactly"), TurnRates[Index], Step),
				FMath::Abs(Velocity.Size() - M5_030_SpeedCmS) < 0.01);
			const float TurnedDeg = FMath::RadiansToDegrees(FMath::Acos(
				FMath::Clamp(FVector::DotProduct(PrevStepDirections[Index], StepDirection), -1.0f, 1.0f)));
			PrevStepDirections[Index] = StepDirection;
			TestTrue(FString::Printf(TEXT("turn-%.0f step %d turned %.2f deg, within the configured rate"),
				TurnRates[Index], Step, TurnedDeg), TurnedDeg <= MaxTurnDegPerStep[Index] + 0.1f);
		}
		if (!bAnyAlive)
		{
			break;
		}
	}

	// Both pellets curved onto their targets and hit exactly once; the higher
	// turn rate curved onto the target strictly sooner (the curvature follows
	// the configuration).
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TestTrue(FString::Printf(TEXT("the turn-%.0f pellet finished on its target within the budget"), TurnRates[Index]),
			FinishSteps[Index] > 0 && Policies[Index].IsFinished());
		TestEqual(FString::Printf(TEXT("the turn-%.0f target took exactly one hit"), TurnRates[Index]),
			TargetHealths[Index]->GetHealth(), 93.0f);
		TestEqual(FString::Printf(TEXT("the turn-%.0f pellet consumed exactly one passage"), TurnRates[Index]),
			Policies[Index].GetNumPiercedHits(), 1);
	}
	TestTrue(FString::Printf(TEXT("step 10: the higher rate bears %.1f deg onto the target vs %.1f deg"),
		BearingAfterStep10[1], BearingAfterStep10[0]),
		BearingAfterStep10[0] >= 0.0f && BearingAfterStep10[1] >= 0.0f && BearingAfterStep10[1] < BearingAfterStep10[0]);
	TestTrue(FString::Printf(TEXT("the higher turn rate curved onto the target strictly sooner (%d < %d steps)"),
		FinishSteps[1], FinishSteps[0]), FinishSteps[1] < FinishSteps[0]);
	return true;
}

// ---------------------------------------------------------------------------
// MovingTargetPursuedToHit
// ---------------------------------------------------------------------------

// The pursuit face: a target that keeps sliding away on +X is caught - the
// pellet's bounded-turn guidance closes the bearing and lands exactly one
// unified hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_030MovingTargetPursuedToHit,
	"UEMMO.Tasks.M5_030.MovingTargetPursuedToHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_030MovingTargetPursuedToHit::RunTest(const FString& Parameters)
{
	UWorld* World = M5_030_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_030_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_030_Scene Scene;
	if (!Scene.Build(*this, *World, M5_030_SceneBase + FVector(-100.0, 0.0, 0.0)))
	{
		return true;
	}

	AActor* Target = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(1200.0, 600.0, 0.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the target carries a health component"), TargetHealth))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_030_SpawnProjectile(*this, *World, M5_030_SceneBase, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FHomingProjectilePolicy Policy;
	if (!M5_030_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_030_MakeHitContext(Scene),
		Scene.IdOf(*Target), 90.0f, EHomingLostTargetAction::KeepStraight, TEXT("the pursuing pellet")))
	{
		return true;
	}

	// The target slides 60cm per step on +X (far slower than the pellet's
	// 100cm/step); the pursuit must catch it well inside the budget.
	int32 Steps = 0;
	while (Steps < 120 && !Policy.IsFinished())
	{
		Target->SetActorLocation(Target->GetActorLocation() + FVector(60.0, 0.0, 0.0));
		Policy.AdvanceMotion(1.0f / 60.0f);
		++Steps;
	}
	TestTrue(TEXT("the moving target was caught within the budget"), Steps < 120 && Policy.IsFinished());
	TestEqual(TEXT("the moving target took exactly one hit"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("exactly one hostile passage was consumed"), Policy.GetNumPiercedHits(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// UnregisteredTargetLossKeepsStraight
// ---------------------------------------------------------------------------

// The keep-straight lost-target face: a mid-flight unregister releases the
// reference immediately and the pellet keeps flying the exact direction it
// had at the loss (no further guidance, no re-search onto the decoy
// registered behind the flight line).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_030UnregisteredTargetLossKeepsStraight,
	"UEMMO.Tasks.M5_030.UnregisteredTargetLossKeepsStraight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_030UnregisteredTargetLossKeepsStraight::RunTest(const FString& Parameters)
{
	UWorld* World = M5_030_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_030_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_030_Scene Scene;
	if (!Scene.Build(*this, *World, M5_030_SceneBase + FVector(-100.0, 0.0, 0.0)))
	{
		return true;
	}

	AActor* Target = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(1500.0, 500.0, 0.0), TEXT("Enemy"));
	// A registered decoy off the flight line: a re-searching policy would
	// curve onto it after the loss; this pellet must never touch it.
	AActor* Decoy = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(1500.0, -800.0, 0.0), TEXT("Enemy"));
	if (Target == nullptr || Decoy == nullptr)
	{
		return true;
	}
	UHealthComponent* DecoyHealth = Decoy->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the decoy carries a health component"), DecoyHealth))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_030_SpawnProjectile(*this, *World, M5_030_SceneBase, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FHomingProjectilePolicy Policy;
	if (!M5_030_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_030_MakeHitContext(Scene),
		Scene.IdOf(*Target), 90.0f, EHomingLostTargetAction::KeepStraight, TEXT("the losing pellet")))
	{
		return true;
	}

	// Two guided steps first: the guidance is demonstrably active (the pellet
	// has already curved off the pure +X launch line).
	Policy.AdvanceMotion(1.0f / 60.0f);
	Policy.AdvanceMotion(1.0f / 60.0f);
	TestTrue(TEXT("the guidance curved the pellet off the launch line before the loss"),
		Pellet->GetActorLocation().Y > M5_030_SceneBase.Y + 1.0);
	TestFalse(TEXT("the pellet is still alive before the loss"), Policy.IsFinished());

	// The loss: the target unregisters mid-flight.
	const FVector LossLocation = Pellet->GetActorLocation();
	// The loss direction is the pellet's instantaneous flight direction at
	// the loss (the guided arc's tangent), not the chord from the launch
	// point - the keep-straight face continues the tangent exactly.
	const FVector LossDirection = Pellet->Movement->Velocity.GetSafeNormal();
	TestTrue(TEXT("the target unregistered"), Scene.Registry.UnregisterEntity(Scene.IdOf(*Target), Scene.Epoch));

	// Three straight steps after the loss: the exact loss direction, the
	// exact speed*dt displacement, the reference released.
	for (int32 Step = 1; Step <= 3; ++Step)
	{
		Policy.AdvanceMotion(1.0f / 60.0f);
		const FVector Displacement = Pellet->GetActorLocation() - LossLocation;
		const float ExpectedLength = M5_030_SpeedCmS * (1.0f / 60.0f) * Step;
		TestTrue(FString::Printf(TEXT("post-loss step %d flew the loss direction exactly"), Step),
			Displacement.GetSafeNormal().Equals(LossDirection, 0.01));
		TestTrue(FString::Printf(TEXT("post-loss step %d flew exactly speed*dt"), Step),
			FMath::Abs(Displacement.Size() - ExpectedLength) < 0.1);
	}
	TestTrue(TEXT("the loss is observable"), Policy.HasLostTarget());
	TestTrue(TEXT("the bound reference was released"), Policy.GetBoundTargetEntityId() == InvalidCombatEntityId);
	TestFalse(TEXT("the keep-straight pellet stays alive"), Policy.IsFinished());

	// No re-search: the decoy never took a hit and never will from this
	// straight flight.
	TestEqual(TEXT("the decoy never took damage"), DecoyHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("no hostile passage was consumed"), Policy.GetNumPiercedHits(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// DestroyedTargetLossTerminatesMotion
// ---------------------------------------------------------------------------

// The destroy lost-target face: a mid-flight actor destruction releases the
// reference and terminates the pellet's motion on the very next advance - no
// crash, no dangling reference, no further movement, no hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_030DestroyedTargetLossTerminatesMotion,
	"UEMMO.Tasks.M5_030.DestroyedTargetLossTerminatesMotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_030DestroyedTargetLossTerminatesMotion::RunTest(const FString& Parameters)
{
	UWorld* World = M5_030_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_030_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_030_Scene Scene;
	if (!Scene.Build(*this, *World, M5_030_SceneBase + FVector(-100.0, 0.0, 0.0)))
	{
		return true;
	}

	AActor* Target = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(1500.0, 500.0, 0.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_030_SpawnProjectile(*this, *World, M5_030_SceneBase, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FHomingProjectilePolicy Policy;
	if (!M5_030_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_030_MakeHitContext(Scene),
		Scene.IdOf(*Target), 90.0f, EHomingLostTargetAction::Destroy, TEXT("the destroying pellet")))
	{
		return true;
	}

	// Two guided steps, then the target dies mid-flight.
	Policy.AdvanceMotion(1.0f / 60.0f);
	Policy.AdvanceMotion(1.0f / 60.0f);
	TestTrue(TEXT("the pellet is still alive before the loss"), !Policy.IsFinished());
	const FVector LossLocation = Pellet->GetActorLocation();
	TestTrue(TEXT("the target actor destroyed"), Target->Destroy());

	// The very next advance terminates the motion (the destroy action).
	Policy.AdvanceMotion(1.0f / 60.0f);
	TestTrue(TEXT("the destroy action terminated the pellet"), Policy.IsFinished());
	TestTrue(TEXT("the loss is observable"), Policy.HasLostTarget());
	TestTrue(TEXT("the bound reference was released"), Policy.GetBoundTargetEntityId() == InvalidCombatEntityId);
	TestTrue(TEXT("the terminating advance never moved the pellet"),
		Pellet->GetActorLocation().Equals(LossLocation, 0.01));
	TestEqual(TEXT("no hostile passage was consumed"), Policy.GetNumPiercedHits(), 0);

	// No residual motion: every later advance is a no-op.
	Policy.AdvanceMotion(1.0f / 60.0f);
	TestTrue(TEXT("the follow-up advance never moved the terminated pellet"),
		Pellet->GetActorLocation().Equals(LossLocation, 0.01));
	return true;
}

// ---------------------------------------------------------------------------
// TargetRefusalsFriendUnregisteredAndDangling
// ---------------------------------------------------------------------------

// The binding refusal face: a friendly target, an unregistered id, the
// invalid id, a logic-only null-actor record and an unconfigured turn rate
// all refuse Begin with zero side effects - the policy never tracks a
// friendly, never holds a dangling target and never invents one. A legal
// configuration on the same scene begins cleanly (the control).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_030TargetRefusalsFriendUnregisteredAndDangling,
	"UEMMO.Tasks.M5_030.TargetRefusalsFriendUnregisteredAndDangling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_030TargetRefusalsFriendUnregisteredAndDangling::RunTest(const FString& Parameters)
{
	UWorld* World = M5_030_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_030_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_030_Scene Scene;
	if (!Scene.Build(*this, *World, M5_030_SceneBase + FVector(-100.0, 0.0, 0.0)))
	{
		return true;
	}

	// The control: a legal hostile target begins cleanly (this pins the
	// refusal cases below to the binding semantics, not a generally broken
	// Begin - the stub-red stage fails here because the stub refuses
	// everything).
	AActor* LegalTarget = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(1500.0, 0.0, 0.0), TEXT("Enemy"));
	if (LegalTarget == nullptr)
	{
		return true;
	}
	ACombatProjectile* ControlPellet = M5_030_SpawnProjectile(*this, *World, M5_030_SceneBase, Scene.ShooterId);
	if (ControlPellet == nullptr)
	{
		return true;
	}
	FHomingProjectilePolicy ControlPolicy;
	if (!M5_030_Begin(*this, ControlPolicy, *ControlPellet, FVector(1.0, 0.0, 0.0), M5_030_MakeHitContext(Scene),
		Scene.IdOf(*LegalTarget), 90.0f, EHomingLostTargetAction::KeepStraight, TEXT("the control pellet")))
	{
		return true;
	}
	ControlPolicy.AdvanceMotion(1.0f / 60.0f);
	TestFalse(TEXT("the control pellet flies"), ControlPolicy.IsFinished());

	// Refusal lanes (the Y depth axis keeps them disjoint): friendly,
	// unregistered id, invalid id, dangling logic-only record, unconfigured
	// turn rate.
	AActor* FriendlyTarget = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(1500.0, 2000.0, 0.0), TEXT("Player"));
	if (FriendlyTarget == nullptr)
	{
		return true;
	}
	// A logic-only record: registered, but resolves to no actor (the
	// dangling/bare-address prohibition face).
	FCombatEntityMetadata LogicMetadata;
	LogicMetadata.Faction = TEXT("Enemy");
	LogicMetadata.Category = TEXT("logic_only");
	const FEntityId LogicOnlyId = Scene.Registry.RegisterEntity(/*Entity*/ nullptr, LogicMetadata, Scene.Epoch);
	TestTrue(TEXT("the logic-only record registered"), LogicOnlyId != InvalidCombatEntityId);

	const FVector LaneOrigins[5] = {
		M5_030_SceneBase + FVector(0.0, 4000.0, 0.0),   // friendly
		M5_030_SceneBase + FVector(0.0, 6000.0, 0.0),   // unregistered id
		M5_030_SceneBase + FVector(0.0, 8000.0, 0.0),   // invalid id
		M5_030_SceneBase + FVector(0.0, 10000.0, 0.0),  // logic-only record
		M5_030_SceneBase + FVector(0.0, 12000.0, 0.0)   // unconfigured turn rate
	};
	const FEntityId LaneTargets[5] = {
		Scene.IdOf(*FriendlyTarget),
		static_cast<FEntityId>(999999),
		InvalidCombatEntityId,
		LogicOnlyId,
		Scene.IdOf(*LegalTarget) // a legal id, but the turn rate is never configured
	};
	for (int32 Lane = 0; Lane < 5; ++Lane)
	{
		ACombatProjectile* Pellet = M5_030_SpawnProjectile(*this, *World, LaneOrigins[Lane], Scene.ShooterId);
		if (Pellet == nullptr)
		{
			return true;
		}
		FHomingProjectilePolicy Policy;
		Policy.Configure(Lane == 4 ? 0.0f : 90.0f, EHomingLostTargetAction::KeepStraight);
		const bool bBound = Policy.BindTarget(LaneTargets[Lane]);
		if (Lane == 2)
		{
			TestTrue(TEXT("the invalid id refuses BindTarget itself"), !bBound);
		}
		else
		{
			TestTrue(FString::Printf(TEXT("refusal lane %d: the id bound (the Begin face decides)"), Lane), bBound);
		}
		TestTrue(FString::Printf(TEXT("refusal lane %d: Begin refused"), Lane),
			!Policy.Begin(*Pellet, FVector(1.0, 0.0, 0.0), M5_030_MakeHitContext(Scene)));
		TestTrue(FString::Printf(TEXT("refusal lane %d: the refusal left the policy finished"), Lane), Policy.IsFinished());
		TestTrue(FString::Printf(TEXT("refusal lane %d: the refusal never moved the pellet"), Lane),
			Pellet->GetActorLocation().Equals(LaneOrigins[Lane], 0.01));
		TestEqual(FString::Printf(TEXT("refusal lane %d: zero passages"), Lane), Policy.GetNumPiercedHits(), 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// HitOnceContinuousCollisionNoResidual
// ---------------------------------------------------------------------------

// The shared hit and residual faces: the guided pellet terminates ON the
// target's surface (continuous collision - the body never enters the
// target's box interior), submits exactly one unified hit, never activates
// the movement component, never touches the lifetime face, and a destroyed
// pellet leaves the policy a silent no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_030HitOnceContinuousCollisionNoResidual,
	"UEMMO.Tasks.M5_030.HitOnceContinuousCollisionNoResidual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_030HitOnceContinuousCollisionNoResidual::RunTest(const FString& Parameters)
{
	UWorld* World = M5_030_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_030_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_030_Scene Scene;
	if (!Scene.Build(*this, *World, M5_030_SceneBase + FVector(-100.0, 0.0, 0.0)))
	{
		return true;
	}

	// The target sits 5000cm ahead and 900cm off the lane (a ~10 degrees
	// bearing). The convergence precondition is the bearing-rate face: near a
	// target the bearing sweeps at up to (v/r) and a rate-90 pellet only
	// converges while (v/r) stays below omega - the first two runs recorded
	// the fixture defect exactly (a 1552cm lane put (v/r) at 221 deg/s, the
	// pellet circled on its 3820cm turn radius and never converged); 5000cm
	// keeps (v/r) at 67 deg/s < 90. Fixture-geometry fix only, assertions
	// unchanged in meaning.
	AActor* Target = M5_030_SpawnRegisteredTarget(*this, *World, Scene,
		M5_030_SceneBase + FVector(5000.0, 900.0, 0.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the target carries a health component"), TargetHealth))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_030_SpawnProjectile(*this, *World, M5_030_SceneBase, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FHomingProjectilePolicy Policy;
	if (!M5_030_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_030_MakeHitContext(Scene),
		Scene.IdOf(*Target), 90.0f, EHomingLostTargetAction::KeepStraight, TEXT("the striking pellet")))
	{
		return true;
	}

	// No residual motion callbacks from Begin: the movement component was
	// configured, never activated; the lifetime face was never touched.
	TestFalse(TEXT("the movement component stays inactive (no registered tick callback)"),
		Pellet->Movement->IsActive());
	TestTrue(TEXT("the policy never touches the lifetime face"),
		FMath::IsNearlyEqual(Pellet->InitialLifeSpan, 0.0f, 0.0001f));

	int32 Steps = 0;
	while (Steps < 120 && !Policy.IsFinished())
	{
		Policy.AdvanceMotion(1.0f / 60.0f);
		++Steps;
	}
	TestTrue(TEXT("the guided pellet finished on its target within the budget"), Steps < 120 && Policy.IsFinished());
	TestEqual(TEXT("the target took exactly one hit"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("exactly one hostile passage was consumed"), Policy.GetNumPiercedHits(), 1);

	// Continuous collision: the body stopped at the target's surface - the
	// pellet center never entered the target's box interior (half extent 30).
	const float CenterDistance = FVector::Distance(Pellet->GetActorLocation(), Target->GetActorLocation());
	TestTrue(FString::Printf(TEXT("the pellet rested on the target surface instead of tunneling (d=%.2f)"), CenterDistance),
		CenterDistance > 30.0);

	// Destroying the pellet leaves the policy a silent no-op (the weak
	// reference goes stale; no callback into a dead actor).
	TestTrue(TEXT("the pellet destroyed"), Pellet->Destroy());
	Policy.AdvanceMotion(1.0f / 60.0f);
	return true;
}

#endif
