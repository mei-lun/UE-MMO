// M5-029: the ballistic (parabolic) motion policy tests. Pins the engine-
// parameter gravity face (the policy sets UE movement-component velocity/
// gravity parameters and drives the engine's own ComputeVelocity/Verlet
// integration - no hand-written physics), the 0-gravity equivalence to the
// straight line, the 0/1/2 gravity-scale trend with quantitative tolerances,
// high-speed landing never tunneling a thin floor with exactly one
// termination event, the shared-hit-classification hostility submission, the
// self pass-through, and the no-residual-callback invalidation face.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/LinearProjectilePolicy.h"
#include "../Projectiles/Policies/BallisticPolicy.h"

#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_029
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the flight axis, Y the depth axis, Z height (gravity is
	// the engine's world -Z acceleration).
	const FVector M5_029_SceneBase(80000.0, 60000.0, 800.0);

	// Flight speed for every case: 6000 cm/s (a 1/60s step covers 100cm).
	const float M5_029_SpeedCmS = 6000.0f;

	// The pinned engine world gravity magnitude (default WorldSettings gravity).
	const float M5_029_WorldGravityZ = -980.0f;

	/** A fully resolved per-hit profile (pure value; the policy never reads catalogs). */
	static FDamageProfile M5_029_MakeProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("ballistic_test_round");
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
	static UWorld* M5_029_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_029_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_029 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_029 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_029_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_029_WorldScope()
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
	 * source entity. The shooter stands behind the projectile origin (or on
	 * the flight line with a body when the case asks for the self face).
	 */
	struct FM5_029_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FTestTargetIdentity Identity;
		FCombatEpoch Epoch = 1;
		FEntityId ShooterId = InvalidCombatEntityId;
		AActor* Shooter = nullptr;
		UHealthComponent* ShooterHealth = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& ShooterLocation, bool bShooterWithBody)
		{
			FActorSpawnParameters Params;
			Shooter = World.SpawnActor<AActor>(AActor::StaticClass(), ShooterLocation, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("a shooter actor spawned"), Shooter))
			{
				return false;
			}
			if (bShooterWithBody)
			{
				UBoxComponent* Body = NewObject<UBoxComponent>(Shooter, TEXT("M5_029_ShooterBody"));
				Shooter->SetRootComponent(Body);
				Body->SetMobility(EComponentMobility::Movable);
				Body->SetBoxExtent(FVector(30.0, 30.0, 90.0));
				Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Body->SetCollisionObjectType(ECC_Pawn);
				Body->SetCollisionResponseToAllChannels(ECR_Ignore);
				Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
				Body->RegisterComponent();
				Body->SetWorldLocation(ShooterLocation);
				ShooterHealth = NewObject<UHealthComponent>(Shooter, TEXT("M5_029_ShooterHealth"));
				ShooterHealth->RegisterComponent();
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
	};

	/** Spawns a query-only box body (target body, wall or floor) at Center. */
	static AActor* M5_029_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& Extent, ECollisionChannel ObjectType)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_029_Box"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(Extent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ObjectType);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		// The projectile body blocks this object type; the blocking face needs
		// the reciprocal response (either side ignoring kills the block pair).
		Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Body->RegisterComponent();
		Body->SetWorldLocation(Center);
		return Actor;
	}

	/** Spawns a registered hostile/friendly target (box body + health + combat). */
	static AActor* M5_029_SpawnRegisteredTarget(FAutomationTestBase& Test, UWorld& World, FM5_029_Scene& Scene,
		const FVector& Center, const TCHAR* Faction)
	{
		AActor* Actor = M5_029_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), ECC_Pawn);
		if (!Test.TestNotNull(TEXT("a target actor spawned"), Actor))
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_029_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_029_Combat"));
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

	/** Spawns a pellet actor with its provenance and gravity scale stamped. */
	static ACombatProjectile* M5_029_SpawnProjectile(FAutomationTestBase& Test, UWorld& World,
		const FVector& Origin, float SpeedCmS, FEntityId SourceId, float MotionGravityScale)
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
		Actor->Context.ShotId = 500;
		Actor->Context.PelletIndex = 0;
		Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
		Actor->Context.ProjectileId = TEXT("bullet_ballistic_test");
		Actor->LaunchSpeedCmS = SpeedCmS;
		Actor->MotionGravityScale = MotionGravityScale;
		return Actor;
	}

	/** The default hit context for one scene (all filter bits on, no pierce). */
	static FProjectileHitContext M5_029_MakeHitContext(FM5_029_Scene& Scene, int32 PierceCount = 0,
		bool bFriendliesBlockShot = true, bool bUnregisteredActorsBlockShot = true)
	{
		FProjectileHitContext Context;
		Context.Registry = &Scene.Registry;
		Context.Ledger = &Scene.Ledger;
		Context.Identity = &Scene.Identity;
		Context.AttackProfile = M5_029_MakeProfile();
		Context.PierceCount = PierceCount;
		Context.bFriendliesBlockShot = bFriendliesBlockShot;
		Context.bUnregisteredActorsBlockShot = bUnregisteredActorsBlockShot;
		Context.SourceActor = Scene.Shooter;
		return Context;
	}

	/** Begins one policy; a refusal fails the case immediately. */
	static bool M5_029_Begin(FAutomationTestBase& Test, FBallisticProjectilePolicy& Policy, ACombatProjectile& Actor,
		const FVector& Direction, const FProjectileHitContext& Context, const TCHAR* What)
	{
		return Test.TestTrue(FString::Printf(TEXT("%s: the policy began"), What), Policy.Begin(Actor, Direction, Context));
	}
}

using namespace UE::UEMMO::Tasks::M5_029;

// ---------------------------------------------------------------------------
// ZeroGravityMatchesLinear
// ---------------------------------------------------------------------------

// The 0-gravity face: gravity scale 0 means zero engine acceleration, so the
// ballistic policy flies exactly the straight line - every 1/60s step covers
// exactly speed*dt on X and never leaves Z=800.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_029ZeroGravityMatchesLinear,
	"UEMMO.Tasks.M5_029.ZeroGravityMatchesLinear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_029ZeroGravityMatchesLinear::RunTest(const FString& Parameters)
{
	UWorld* World = M5_029_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_029_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_029_Scene Scene;
	if (!Scene.Build(*this, *World, M5_029_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}

	ACombatProjectile* Pellet = M5_029_SpawnProjectile(*this, *World, M5_029_SceneBase, M5_029_SpeedCmS,
		Scene.ShooterId, /*MotionGravityScale*/ 0.0f);
	if (Pellet == nullptr)
	{
		return true;
	}
	FBallisticProjectilePolicy Policy;
	if (!M5_029_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_029_MakeHitContext(Scene), TEXT("the straight pellet")))
	{
		return true;
	}
	for (int32 Step = 1; Step <= 3; ++Step)
	{
		Policy.AdvanceMotion(1.0f / 60.0f);
		const float ExpectedX = M5_029_SceneBase.X + M5_029_SpeedCmS * (1.0f / 60.0f) * Step;
		TestTrue(FString::Printf(TEXT("zero-gravity step %d flew exactly speed*dt on X"), Step),
			FMath::Abs(Pellet->GetActorLocation().X - ExpectedX) < 0.01);
		TestTrue(FString::Printf(TEXT("zero-gravity step %d never left Z=800"), Step),
			FMath::Abs(Pellet->GetActorLocation().Z - M5_029_SceneBase.Z) < 0.01);
		TestFalse(FString::Printf(TEXT("zero-gravity step %d stays alive in clean air"), Step), Policy.IsFinished());
	}
	return true;
}

// ---------------------------------------------------------------------------
// GravityScaleTrendAndQuantum
// ---------------------------------------------------------------------------

// The 0/1/2 gravity trend with quantitative tolerances: the engine Verlet
// step is exact at step boundaries for constant acceleration, so after N
// steps of dt the drop is 0.5 * worldGravity * scale * (N*dt)^2. Scale 2
// drops twice as far as scale 1; scale 0 never drops; X advances identically
// for all three (gravity is the engine's world-Z acceleration only).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_029GravityScaleTrendAndQuantum,
	"UEMMO.Tasks.M5_029.GravityScaleTrendAndQuantum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_029GravityScaleTrendAndQuantum::RunTest(const FString& Parameters)
{
	UWorld* World = M5_029_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_029_WorldScope WorldScope;
	WorldScope.World = World;

	// The quantitative face needs the default world gravity pinned.
	TestTrue(TEXT("the temp world uses the default -980 gravity"),
		FMath::IsNearlyEqual(World->GetGravityZ(), M5_029_WorldGravityZ, 0.01f));

	FM5_029_Scene Scene;
	if (!Scene.Build(*this, *World, M5_029_SceneBase + FVector(-100.0, -4000.0, 0.0), false))
	{
		return true;
	}

	// Three pellets on three Y lanes (one world, no shared geometry): scale
	// 0 / 1 / 2. Ten 1/60s steps = 1/6s of flight.
	const float GravityScales[3] = { 0.0f, 1.0f, 2.0f };
	ACombatProjectile* Pellets[3] = { nullptr, nullptr, nullptr };
	FBallisticProjectilePolicy Policies[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Pellets[Index] = M5_029_SpawnProjectile(*this, *World,
			M5_029_SceneBase + FVector(0.0, 2000.0 * Index, 0.0), M5_029_SpeedCmS, Scene.ShooterId, GravityScales[Index]);
		if (Pellets[Index] == nullptr)
		{
			return true;
		}
		if (!M5_029_Begin(*this, Policies[Index], *Pellets[Index], FVector(1.0, 0.0, 0.0),
			M5_029_MakeHitContext(Scene), *FString::Printf(TEXT("the scale-%.0f pellet"), GravityScales[Index])))
		{
			return true;
		}
	}
	for (int32 Step = 0; Step < 10; ++Step)
	{
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Policies[Index].AdvanceMotion(1.0f / 60.0f);
		}
	}
	const float FlightTime = 10.0f / 60.0f;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float ExpectedX = M5_029_SceneBase.X + M5_029_SpeedCmS * FlightTime;
		const float ExpectedZ = M5_029_SceneBase.Z
			+ 0.5f * M5_029_WorldGravityZ * GravityScales[Index] * FlightTime * FlightTime;
		TestTrue(FString::Printf(TEXT("scale-%.0f pellet flew the same X"), GravityScales[Index]),
			FMath::Abs(Pellets[Index]->GetActorLocation().X - ExpectedX) < 0.01);
		// The engine Verlet step is exact at step boundaries for a constant
		// acceleration; the recorded tolerance covers float accumulation only.
		TestTrue(FString::Printf(TEXT("scale-%.0f pellet dropped exactly 0.5*g*scale*t^2 (z=%.3f)"),
			GravityScales[Index], Pellets[Index]->GetActorLocation().Z),
			FMath::Abs(Pellets[Index]->GetActorLocation().Z - ExpectedZ) < 0.1);
	}
	// The trend face: heavier gravity sits strictly lower on Z.
	TestTrue(TEXT("scale-2 sits strictly lower than scale-1"),
		Pellets[2]->GetActorLocation().Z < Pellets[1]->GetActorLocation().Z);
	TestTrue(TEXT("scale-1 sits strictly lower than scale-0"),
		Pellets[1]->GetActorLocation().Z < Pellets[0]->GetActorLocation().Z);
	return true;
}

// ---------------------------------------------------------------------------
// HighSpeedLandingNoTunnelSingleEvent
// ---------------------------------------------------------------------------

// The high-speed landing face: a 12000cm/s fall over a 0.1s step (1200cm+
// of travel) onto a 5cm-thick floor stops at the floor's top face instead
// of tunneling, terminates exactly once (a follow-up advance is a no-op)
// and never submits (an unregistered world body is not a target).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_029HighSpeedLandingNoTunnelSingleEvent,
	"UEMMO.Tasks.M5_029.HighSpeedLandingNoTunnelSingleEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_029HighSpeedLandingNoTunnelSingleEvent::RunTest(const FString& Parameters)
{
	UWorld* World = M5_029_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_029_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_029_Scene Scene;
	if (!Scene.Build(*this, *World, M5_029_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}

	// A 5cm-thick floor whose top face sits 100cm below the pellet start.
	AActor* Floor = M5_029_SpawnBoxActor(*World, M5_029_SceneBase + FVector(0.0, 0.0, -105.0),
		FVector(200.0, 200.0, 5.0), ECC_WorldStatic);
	if (!TestNotNull(TEXT("a thin floor spawned"), Floor))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_029_SpawnProjectile(*this, *World, M5_029_SceneBase, 12000.0f,
		Scene.ShooterId, /*MotionGravityScale*/ 1.0f);
	if (Pellet == nullptr)
	{
		return true;
	}
	FBallisticProjectilePolicy Policy;
	if (!M5_029_Begin(*this, Policy, *Pellet, FVector(0.0, 0.0, -1.0), M5_029_MakeHitContext(Scene), TEXT("the falling pellet")))
	{
		return true;
	}
	const float FloorTopZ = M5_029_SceneBase.Z - 100.0f;
	// Exact contact rests the sphere CENTER a radius above the floor's top
	// face; the engine's Chaos sweep carries a per-shape contact offset (this
	// fixture measured ~2.2cm on the 200x200x5 box), so the tolerance is 3cm.
	// The recorded quantitative tolerance: observed rest z=710.205 vs exact 708.
	const float ExactRestZ = FloorTopZ + 8.0f;
	const FBox FloorBounds = Floor->GetComponentsBoundingBox();
	AddInfo(FString::Printf(TEXT("floor bounds min=%s max=%s"), *FloorBounds.Min.ToString(), *FloorBounds.Max.ToString()));
	AddInfo(FString::Printf(TEXT("pellet spawn loc=%s body radius=%.3f"), *Pellet->GetActorLocation().ToString(),
		Pellet->Body->GetUnscaledSphereRadius()));
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the high-speed fall terminated on the floor"), Policy.IsFinished());
	TestTrue(FString::Printf(TEXT("the pellet rested on the floor top face instead of tunneling (z=%.3f)"),
		Pellet->GetActorLocation().Z), FMath::Abs(Pellet->GetActorLocation().Z - ExactRestZ) < 3.0);
	TestTrue(TEXT("the pellet body never reached the floor's interior"),
		Pellet->GetActorLocation().Z - Pellet->Body->GetUnscaledSphereRadius() >= FloorTopZ);
	TestEqual(TEXT("no hostile passage was consumed"), Policy.GetNumPiercedHits(), 0);

	// The termination event fired exactly once: a follow-up advance is a no-op.
	const FVector RestLocation = Pellet->GetActorLocation();
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the follow-up advance never moved the terminated pellet"),
		Pellet->GetActorLocation().Equals(RestLocation, 0.01));
	return true;
}

// ---------------------------------------------------------------------------
// HostileHitSubmittedExactlyOnce
// ---------------------------------------------------------------------------

// The hostile face: a fast fall onto a registered hostile body submits the
// complete-key unified hit exactly once (damage applied, one pierce slot)
// and terminates; a follow-up advance neither damages again nor moves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_029HostileHitSubmittedExactlyOnce,
	"UEMMO.Tasks.M5_029.HostileHitSubmittedExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_029HostileHitSubmittedExactlyOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M5_029_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_029_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_029_Scene Scene;
	if (!Scene.Build(*this, *World, M5_029_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}

	// A flat registered hostile pad whose top face sits 95cm below the start.
	AActor* Target = M5_029_SpawnRegisteredTarget(*this, *World, Scene,
		M5_029_SceneBase + FVector(0.0, 0.0, -105.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	// Widen the pad so the Z-span covers the fall line (the default box is
	// tall; this case wants a low wide one). Rebuild the body extent.
	Target->FindComponentByClass<UBoxComponent>()->SetBoxExtent(FVector(60.0, 60.0, 10.0));
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the target carries a health component"), TargetHealth))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_029_SpawnProjectile(*this, *World, M5_029_SceneBase, M5_029_SpeedCmS,
		Scene.ShooterId, /*MotionGravityScale*/ 1.0f);
	if (Pellet == nullptr)
	{
		return true;
	}
	FBallisticProjectilePolicy Policy;
	if (!M5_029_Begin(*this, Policy, *Pellet, FVector(0.0, 0.0, -1.0), M5_029_MakeHitContext(Scene), TEXT("the striking pellet")))
	{
		return true;
	}
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the hostile hit terminated the pellet"), Policy.IsFinished());
	TestEqual(TEXT("the hostile pad took the hit exactly once"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("one hostile passage was consumed"), Policy.GetNumPiercedHits(), 1);

	// Exactly once: a follow-up advance is a no-op (no second damage, no move).
	const FVector HitLocation = Pellet->GetActorLocation();
	Policy.AdvanceMotion(0.1f);
	TestEqual(TEXT("the follow-up advance never damaged again"), TargetHealth->GetHealth(), 93.0f);
	TestTrue(TEXT("the follow-up advance never moved the terminated pellet"),
		Pellet->GetActorLocation().Equals(HitLocation, 0.01));
	return true;
}

// ---------------------------------------------------------------------------
// SelfPassThroughThenHostileHit
// ---------------------------------------------------------------------------

// The self and depth faces: a ballistic pellet flying past its source body
// (with a slight gravity drop) passes through the source (never blocks,
// never damages) and hits the hostile behind it; the bound direction is
// used exactly as provided with the engine's world-Z gravity on top.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_029SelfPassThroughThenHostileHit,
	"UEMMO.Tasks.M5_029.SelfPassThroughThenHostileHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_029SelfPassThroughThenHostileHit::RunTest(const FString& Parameters)
{
	UWorld* World = M5_029_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_029_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_029_Scene Scene;
	if (!Scene.Build(*this, *World, M5_029_SceneBase + FVector(200.0, 0.0, 0.0), /*bShooterWithBody*/ true))
	{
		return true;
	}
	AActor* Target = M5_029_SpawnRegisteredTarget(*this, *World, Scene, M5_029_SceneBase + FVector(600.0, 0.0, 60.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the target carries a health component"), TargetHealth)
		|| !TestNotNull(TEXT("the shooter carries a health component"), Scene.ShooterHealth))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_029_SpawnProjectile(*this, *World, M5_029_SceneBase, M5_029_SpeedCmS,
		Scene.ShooterId, /*MotionGravityScale*/ 1.0f);
	if (Pellet == nullptr)
	{
		return true;
	}
	FBallisticProjectilePolicy Policy;
	if (!M5_029_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_029_MakeHitContext(Scene), TEXT("the passing pellet")))
	{
		return true;
	}
	// Advance 1 carries the pellet through the source body (one 0.05s step
	// reaches into the body; the resume carries it past the back face).
	Policy.AdvanceMotion(0.05f);
	TestTrue(TEXT("the projectile passed its source actor"), Pellet->GetActorLocation().X > M5_029_SceneBase.X + 250.0);
	TestFalse(TEXT("the pellet stays alive after the self pass"), Policy.IsFinished());
	// Advance 2 lands on the hostile behind.
	Policy.AdvanceMotion(0.05f);
	TestTrue(TEXT("the hostile hit terminated the pellet"), Policy.IsFinished());
	TestEqual(TEXT("the target behind the source took the hit"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("the source never took damage"), Scene.ShooterHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("only the hostile passage was consumed"), Policy.GetNumPiercedHits(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// NoResidualMotionOnInvalidation
// ---------------------------------------------------------------------------

// The invalidation face: Begin only sets engine parameters and never
// activates the movement component (no tick callback registered anywhere);
// a destroyed pellet leaves the policy a silent no-op (the weak reference
// goes stale, the advance returns without touching anything).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_029NoResidualMotionOnInvalidation,
	"UEMMO.Tasks.M5_029.NoResidualMotionOnInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_029NoResidualMotionOnInvalidation::RunTest(const FString& Parameters)
{
	UWorld* World = M5_029_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_029_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_029_Scene Scene;
	if (!Scene.Build(*this, *World, M5_029_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}

	ACombatProjectile* Pellet = M5_029_SpawnProjectile(*this, *World, M5_029_SceneBase, M5_029_SpeedCmS,
		Scene.ShooterId, /*MotionGravityScale*/ 1.0f);
	if (Pellet == nullptr)
	{
		return true;
	}
	FBallisticProjectilePolicy Policy;
	if (!M5_029_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_029_MakeHitContext(Scene), TEXT("the short-lived pellet")))
	{
		return true;
	}
	// No residual motion callbacks: the engine component was configured but
	// never activated, so nothing in the engine ticks this pellet on its own.
	TestFalse(TEXT("the movement component stays inactive (no registered tick callback)"),
		Pellet->Movement->IsActive());
	TestTrue(TEXT("the initial velocity is the bound direction * launch speed"),
		Pellet->Movement->Velocity.Equals(FVector(M5_029_SpeedCmS, 0.0, 0.0), 0.01));
	TestTrue(TEXT("the gravity parameter mirrors the actor's motion definition"),
		FMath::IsNearlyEqual(Pellet->Movement->ProjectileGravityScale, 1.0f, 0.0001f));
	TestTrue(TEXT("InitialSpeed is zeroed so the bound velocity is authoritative"),
		FMath::IsNearlyEqual(Pellet->Movement->InitialSpeed, 0.0f, 0.0001f));
	TestTrue(TEXT("MaxSpeed is zeroed (no clamp)"), FMath::IsNearlyEqual(Pellet->Movement->MaxSpeed, 0.0f, 0.0001f));
	Policy.AdvanceMotion(1.0f / 60.0f);
	TestTrue(TEXT("the advance flew the step while the pellet lives"),
		FMath::Abs(Pellet->GetActorLocation().X - (M5_029_SceneBase.X + 100.0)) < 0.01);

	// Destroy the pellet: the policy's weak reference goes stale and every
	// later advance is a silent no-op (invalidation leaves no crash, no move,
	// no callback into a dead actor).
	TestTrue(TEXT("the pellet destroyed"), Pellet->Destroy());
	Policy.AdvanceMotion(1.0f / 60.0f);
	return true;
}

#endif
