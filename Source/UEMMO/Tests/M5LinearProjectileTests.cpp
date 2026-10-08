// M5-028: the linear projectile policy (owner 028, the Projectiles/* motion
// face). Pins the one-advancer sweep transport (previous->current body-radius
// sweep through the UE motion component, no tunneling at 30/60/120 FPS steps
// or a 100ms hitch), the shared hit classification (self never blocks,
// friendlies/world block-or-pass per filter), the collision-to-unified-hit
// conversion (complete five-tuple key, one hit result, no direct health
// writes), the pierce-then-terminate config semantics and the Y depth axis
// keeping its full role.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/LinearProjectilePolicy.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_028
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the flight axis, Y the depth axis, Z height.
	const FVector M5_028_SceneBase(80000.0, 60000.0, 800.0);

	// Flight speed for every case: 6000 cm/s (a 0.1s step covers 600cm).
	const float M5_028_SpeedCmS = 6000.0f;

	/** A fully resolved per-hit profile (pure value; the policy never reads catalogs). */
	static FDamageProfile M5_028_MakeProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("hitscan_test_round");
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
	static UWorld* M5_028_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_028_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_028 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_028 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_028_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_028_WorldScope()
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
	struct FM5_028_Scene
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
				UBoxComponent* Body = NewObject<UBoxComponent>(Shooter, TEXT("M5_028_ShooterBody"));
				Shooter->SetRootComponent(Body);
				Body->SetMobility(EComponentMobility::Movable);
				Body->SetBoxExtent(FVector(30.0, 30.0, 90.0));
				Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Body->SetCollisionObjectType(ECC_Pawn);
				Body->SetCollisionResponseToAllChannels(ECR_Ignore);
				Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
				Body->RegisterComponent();
				Body->SetWorldLocation(ShooterLocation);
				ShooterHealth = NewObject<UHealthComponent>(Shooter, TEXT("M5_028_ShooterHealth"));
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

	/** Spawns a query-only box body (target body or wall) at Center. */
	static AActor* M5_028_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& Extent, ECollisionChannel ObjectType)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_028_Box"));
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
	static AActor* M5_028_SpawnRegisteredTarget(FAutomationTestBase& Test, UWorld& World, FM5_028_Scene& Scene,
		const FVector& Center, const TCHAR* Faction)
	{
		AActor* Actor = M5_028_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), ECC_Pawn);
		if (!Test.TestNotNull(TEXT("a target actor spawned"), Actor))
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_028_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_028_Combat"));
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

	/** Spawns a pellet actor with its provenance stamped (a value snapshot). */
	static ACombatProjectile* M5_028_SpawnProjectile(FAutomationTestBase& Test, UWorld& World,
		const FVector& Origin, float SpeedCmS, FEntityId SourceId)
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
		Actor->Context.ProjectileId = TEXT("bullet_linear_test");
		Actor->LaunchSpeedCmS = SpeedCmS;
		return Actor;
	}

	/** The default hit context for one scene (all filter bits on, no pierce). */
	static FProjectileHitContext M5_028_MakeHitContext(FM5_028_Scene& Scene, int32 PierceCount = 0,
		bool bFriendliesBlockShot = true, bool bUnregisteredActorsBlockShot = true)
	{
		FProjectileHitContext Context;
		Context.Registry = &Scene.Registry;
		Context.Ledger = &Scene.Ledger;
		Context.Identity = &Scene.Identity;
		Context.AttackProfile = M5_028_MakeProfile();
		Context.PierceCount = PierceCount;
		Context.bFriendliesBlockShot = bFriendliesBlockShot;
		Context.bUnregisteredActorsBlockShot = bUnregisteredActorsBlockShot;
		Context.SourceActor = Scene.Shooter;
		return Context;
	}

	/** Begins one policy; a refusal fails the case immediately. */
	static bool M5_028_Begin(FAutomationTestBase& Test, FLinearProjectilePolicy& Policy, ACombatProjectile& Actor,
		const FVector& Direction, const FProjectileHitContext& Context, const TCHAR* What)
	{
		return Test.TestTrue(FString::Printf(TEXT("%s: the policy began"), What), Policy.Begin(Actor, Direction, Context));
	}
}

using namespace UE::UEMMO::Tasks::M5_028;

// ---------------------------------------------------------------------------
// SweepNoTunnelThinWall
// ---------------------------------------------------------------------------

// The transport face: 30/60/120 FPS steps fly exactly speed*dt through clean
// air; a 100ms hitch (600cm step) stops at a 5cm-thick wall instead of
// tunneling it, and a finished pellet never moves again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_028SweepNoTunnelThinWall,
	"UEMMO.Tasks.M5_028.SweepNoTunnelThinWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_028SweepNoTunnelThinWall::RunTest(const FString& Parameters)
{
	UWorld* World = M5_028_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_028_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_028_Scene Scene;
	if (!Scene.Build(*this, *World, M5_028_SceneBase + FVector(-100.0, 0.0, 0.0), /*bShooterWithBody*/ false))
	{
		return true;
	}

	// Mid-flight: the frame steps cover exactly speed*dt with no hit.
	const float MidDeltas[3] = { 1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ACombatProjectile* Pellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
		if (Pellet == nullptr)
		{
			return true;
		}
		FLinearProjectilePolicy Policy;
		if (!M5_028_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_028_MakeHitContext(Scene), TEXT("the mid-flight pellet")))
		{
			return true;
		}
		Policy.AdvanceMotion(MidDeltas[Index]);
		const float ExpectedX = M5_028_SceneBase.X + M5_028_SpeedCmS * MidDeltas[Index];
		TestTrue(FString::Printf(TEXT("frame step %d flew exactly speed*dt"), Index),
			FMath::Abs(Pellet->GetActorLocation().X - ExpectedX) < 0.01);
		TestFalse(FString::Printf(TEXT("frame step %d stays alive in clean air"), Index), Policy.IsFinished());
	}

	// The hitch face: one 100ms step (600cm) into a 5cm wall at 500 stops at
	// the front face - never tunnels.
	AActor* Wall = M5_028_SpawnBoxActor(*World, M5_028_SceneBase + FVector(500.0, 0.0, 100.0),
		FVector(5.0, 200.0, 200.0), ECC_WorldStatic);
	if (!TestNotNull(TEXT("a thin wall spawned"), Wall))
	{
		return true;
	}
	ACombatProjectile* Pellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy Policy;
	if (!M5_028_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_028_MakeHitContext(Scene), TEXT("the hitch pellet")))
	{
		return true;
	}
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the 100ms step stopped at the thin wall front face"),
		Pellet->GetActorLocation().X <= M5_028_SceneBase.X + 495.0 + 1.0);
	TestTrue(TEXT("the wall hit terminated the pellet"), Policy.IsFinished());

	// A finished pellet never moves again (no second motion, no drift).
	const FVector Stopped = Pellet->GetActorLocation();
	Policy.AdvanceMotion(0.1f);
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("a finished pellet never moves again"), Pellet->GetActorLocation().Equals(Stopped, 0.01));
	return true;
}

// ---------------------------------------------------------------------------
// HostileHitUnifiedExactlyOnce
// ---------------------------------------------------------------------------

// The hit face: one hostile passage submits the unified hit once (the profile
// damage applies exactly once), terminates the pellet (PierceCount 0) and a
// second advance is a full no-op - one motion advancer, one hit result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_028HostileHitUnifiedExactlyOnce,
	"UEMMO.Tasks.M5_028.HostileHitUnifiedExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_028HostileHitUnifiedExactlyOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M5_028_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_028_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_028_Scene Scene;
	if (!Scene.Build(*this, *World, M5_028_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}
	AActor* Target = M5_028_SpawnRegisteredTarget(*this, *World, Scene, M5_028_SceneBase + FVector(400.0, 0.0, 60.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the target carries a health component"), TargetHealth))
	{
		return true;
	}

	ACombatProjectile* Pellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy Policy;
	if (!M5_028_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_028_MakeHitContext(Scene), TEXT("the hostile pellet")))
	{
		return true;
	}
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the hostile hit terminated the pellet"), Policy.IsFinished());
	TestEqual(TEXT("the profile damage applied exactly once"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("one hostile passage was consumed"), Policy.GetNumPiercedHits(), 1);

	const FVector Stopped = Pellet->GetActorLocation();
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("a terminated pellet never moves again"), Pellet->GetActorLocation().Equals(Stopped, 0.01));
	TestEqual(TEXT("no second hit result"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("still one hostile passage"), Policy.GetNumPiercedHits(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// WorldHitTerminatesWithoutSubmission
// ---------------------------------------------------------------------------

// The world face: an unregistered wall blocks and terminates the pellet with
// zero hostile passages (no submission is even formed - the wall carries no
// identity, so the classification can never invent a target).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_028WorldHitTerminatesWithoutSubmission,
	"UEMMO.Tasks.M5_028.WorldHitTerminatesWithoutSubmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_028WorldHitTerminatesWithoutSubmission::RunTest(const FString& Parameters)
{
	UWorld* World = M5_028_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_028_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_028_Scene Scene;
	if (!Scene.Build(*this, *World, M5_028_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}
	AActor* Wall = M5_028_SpawnBoxActor(*World, M5_028_SceneBase + FVector(500.0, 0.0, 100.0),
		FVector(10.0, 200.0, 200.0), ECC_WorldStatic);
	if (!TestNotNull(TEXT("a wall spawned"), Wall))
	{
		return true;
	}

	ACombatProjectile* Pellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy Policy;
	if (!M5_028_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_028_MakeHitContext(Scene), TEXT("the wall pellet")))
	{
		return true;
	}
	Policy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the wall terminated the pellet"), Policy.IsFinished());
	TestTrue(TEXT("the pellet stopped at the wall"),
		Pellet->GetActorLocation().X <= M5_028_SceneBase.X + 490.0 + 1.0);
	TestEqual(TEXT("no hostile passage was consumed on a wall"), Policy.GetNumPiercedHits(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// SelfNeverBlocksNeverDamages
// ---------------------------------------------------------------------------

// The self face: the source actor's own body sits on the flight line but the
// pellet passes through it (the body ignores its source while moving), the
// hostile target behind takes the hit and the source never takes damage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_028SelfNeverBlocksNeverDamages,
	"UEMMO.Tasks.M5_028.SelfNeverBlocksNeverDamages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_028SelfNeverBlocksNeverDamages::RunTest(const FString& Parameters)
{
	UWorld* World = M5_028_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_028_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_028_Scene Scene;
	if (!Scene.Build(*this, *World, M5_028_SceneBase + FVector(200.0, 0.0, 60.0), /*bShooterWithBody*/ true))
	{
		return true;
	}
	AActor* Target = M5_028_SpawnRegisteredTarget(*this, *World, Scene, M5_028_SceneBase + FVector(600.0, 0.0, 60.0), TEXT("Enemy"));
	if (Target == nullptr)
	{
		return true;
	}
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the target carries a health component"), TargetHealth))
	{
		return true;
	}

	ACombatProjectile* Pellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy Policy;
	if (!M5_028_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_028_MakeHitContext(Scene), TEXT("the self pellet")))
	{
		return true;
	}
	Policy.AdvanceMotion(0.2f);
	TestTrue(TEXT("the projectile passed its source actor"), Pellet->GetActorLocation().X > M5_028_SceneBase.X + 300.0);
	TestTrue(TEXT("the hostile hit terminated the pellet"), Policy.IsFinished());
	TestEqual(TEXT("the target behind the source took the hit"), TargetHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("the source never took damage"), Scene.ShooterHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("only the hostile passage was consumed"), Policy.GetNumPiercedHits(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// FriendlyPerFilterBlockOrPass
// ---------------------------------------------------------------------------

// The friendly face: with the block bit the same-faction body stops the pellet
// and nobody takes damage; with the pass bit the pellet flies through the
// friendly (still undamaged) and hits the hostile behind it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_028FriendlyPerFilterBlockOrPass,
	"UEMMO.Tasks.M5_028.FriendlyPerFilterBlockOrPass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_028FriendlyPerFilterBlockOrPass::RunTest(const FString& Parameters)
{
	UWorld* World = M5_028_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_028_WorldScope WorldScope;
	WorldScope.World = World;

	// Block variant (default filter): the friendly body stops the pellet.
	FM5_028_Scene BlockScene;
	if (!BlockScene.Build(*this, *World, M5_028_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}
	AActor* Friendly = M5_028_SpawnRegisteredTarget(*this, *World, BlockScene, M5_028_SceneBase + FVector(300.0, 0.0, 60.0), TEXT("Player"));
	AActor* Hostile = M5_028_SpawnRegisteredTarget(*this, *World, BlockScene, M5_028_SceneBase + FVector(600.0, 0.0, 60.0), TEXT("Enemy"));
	if (Friendly == nullptr || Hostile == nullptr)
	{
		return true;
	}
	UHealthComponent* FriendlyHealth = Friendly->FindComponentByClass<UHealthComponent>();
	UHealthComponent* HostileHealth = Hostile->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the friendly carries a health component"), FriendlyHealth)
		|| !TestNotNull(TEXT("the hostile carries a health component"), HostileHealth))
	{
		return true;
	}
	ACombatProjectile* BlockPellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, BlockScene.ShooterId);
	if (BlockPellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy BlockPolicy;
	if (!M5_028_Begin(*this, BlockPolicy, *BlockPellet, FVector(1.0, 0.0, 0.0),
		M5_028_MakeHitContext(BlockScene), TEXT("the blocking pellet")))
	{
		return true;
	}
	BlockPolicy.AdvanceMotion(0.2f);
	TestTrue(TEXT("the friendly body blocked the pellet"), BlockPolicy.IsFinished());
	TestTrue(TEXT("the pellet stopped at the friendly body"),
		BlockPellet->GetActorLocation().X <= M5_028_SceneBase.X + 300.0 + 1.0);
	TestEqual(TEXT("the friendly never took damage"), FriendlyHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("the hostile behind stays full"), HostileHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("no hostile passage was consumed"), BlockPolicy.GetNumPiercedHits(), 0);

	// Pass variant (offset 2000cm along the Y depth axis so the two variants
	// never share world-space bodies - a body from the block scene would be
	// unregistered in the pass scene's identity seam): the pellet flies
	// through the friendly (undamaged) into the hostile behind it.
	FM5_028_Scene PassScene;
	if (!PassScene.Build(*this, *World, M5_028_SceneBase + FVector(-200.0, 2000.0, 0.0), false))
	{
		return true;
	}
	AActor* PassFriendly = M5_028_SpawnRegisteredTarget(*this, *World, PassScene, M5_028_SceneBase + FVector(300.0, 2000.0, 60.0), TEXT("Player"));
	AActor* PassHostile = M5_028_SpawnRegisteredTarget(*this, *World, PassScene, M5_028_SceneBase + FVector(600.0, 2000.0, 60.0), TEXT("Enemy"));
	if (PassFriendly == nullptr || PassHostile == nullptr)
	{
		return true;
	}
	UHealthComponent* PassFriendlyHealth = PassFriendly->FindComponentByClass<UHealthComponent>();
	UHealthComponent* PassHostileHealth = PassHostile->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the pass friendly carries a health component"), PassFriendlyHealth)
		|| !TestNotNull(TEXT("the pass hostile carries a health component"), PassHostileHealth))
	{
		return true;
	}
	ACombatProjectile* PassPellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase + FVector(0.0, 2000.0, 0.0), M5_028_SpeedCmS, PassScene.ShooterId);
	if (PassPellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy PassPolicy;
	if (!M5_028_Begin(*this, PassPolicy, *PassPellet, FVector(1.0, 0.0, 0.0),
		M5_028_MakeHitContext(PassScene, /*PierceCount*/ 0, /*bFriendliesBlockShot*/ false), TEXT("the passing pellet")))
	{
		return true;
	}
	PassPolicy.AdvanceMotion(0.2f);
	TestTrue(TEXT("the pellet passed the friendly and hit the hostile"), PassPolicy.IsFinished());
	TestEqual(TEXT("the friendly still never took damage"), PassFriendlyHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("the hostile behind took the hit"), PassHostileHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("one hostile passage was consumed"), PassPolicy.GetNumPiercedHits(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// PierceThenTerminateAndYDepthPreserved
// ---------------------------------------------------------------------------

// The pierce and depth faces: PierceCount=1 carries the pellet through the
// first hostile into the second and terminates there; a depth-offset body off
// the line is never hit; a +Y flight direction hits a body standing on the
// depth line - Y keeps its full role.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_028PierceThenTerminateAndYDepthPreserved,
	"UEMMO.Tasks.M5_028.PierceThenTerminateAndYDepthPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_028PierceThenTerminateAndYDepthPreserved::RunTest(const FString& Parameters)
{
	UWorld* World = M5_028_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_028_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_028_Scene Scene;
	if (!Scene.Build(*this, *World, M5_028_SceneBase + FVector(-100.0, 0.0, 0.0), false))
	{
		return true;
	}
	AActor* First = M5_028_SpawnRegisteredTarget(*this, *World, Scene, M5_028_SceneBase + FVector(300.0, 0.0, 60.0), TEXT("Enemy"));
	AActor* Second = M5_028_SpawnRegisteredTarget(*this, *World, Scene, M5_028_SceneBase + FVector(600.0, 0.0, 60.0), TEXT("Enemy"));
	AActor* OffLine = M5_028_SpawnRegisteredTarget(*this, *World, Scene, M5_028_SceneBase + FVector(600.0, 50.0, 60.0), TEXT("Enemy"));
	if (First == nullptr || Second == nullptr || OffLine == nullptr)
	{
		return true;
	}
	UHealthComponent* FirstHealth = First->FindComponentByClass<UHealthComponent>();
	UHealthComponent* SecondHealth = Second->FindComponentByClass<UHealthComponent>();
	UHealthComponent* OffLineHealth = OffLine->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the first target carries a health component"), FirstHealth)
		|| !TestNotNull(TEXT("the second target carries a health component"), SecondHealth)
		|| !TestNotNull(TEXT("the off-line target carries a health component"), OffLineHealth))
	{
		return true;
	}

	ACombatProjectile* Pellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
	if (Pellet == nullptr)
	{
		return true;
	}
	FLinearProjectilePolicy Policy;
	if (!M5_028_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0),
		M5_028_MakeHitContext(Scene, /*PierceCount*/ 1), TEXT("the piercing pellet")))
	{
		return true;
	}
	Policy.AdvanceMotion(0.2f);
	TestEqual(TEXT("the first hit damaged the first target"), FirstHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("the pierce carried the pellet into the second target"), SecondHealth->GetHealth(), 93.0f);
	TestEqual(TEXT("the depth-offset body off the line was never hit"), OffLineHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("two hostile passages consumed the budget"), Policy.GetNumPiercedHits(), 2);
	TestTrue(TEXT("the pellet terminated after the pierce budget"), Policy.IsFinished());

	// The Y depth face: a straight +Y flight hits a body standing on the
	// depth line (the direction is used exactly as provided).
	ACombatProjectile* DepthPellet = M5_028_SpawnProjectile(*this, *World, M5_028_SceneBase, M5_028_SpeedCmS, Scene.ShooterId);
	if (DepthPellet == nullptr)
	{
		return true;
	}
	AActor* DepthTarget = M5_028_SpawnRegisteredTarget(*this, *World, Scene, M5_028_SceneBase + FVector(0.0, 300.0, 60.0), TEXT("Enemy"));
	if (DepthTarget == nullptr)
	{
		return true;
	}
	UHealthComponent* DepthHealth = DepthTarget->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("the depth target carries a health component"), DepthHealth))
	{
		return true;
	}
	FLinearProjectilePolicy DepthPolicy;
	if (!M5_028_Begin(*this, DepthPolicy, *DepthPellet, FVector(0.0, 1.0, 0.0),
		M5_028_MakeHitContext(Scene), TEXT("the depth pellet")))
	{
		return true;
	}
	DepthPolicy.AdvanceMotion(0.1f);
	TestTrue(TEXT("the +Y flight terminated on the depth body"), DepthPolicy.IsFinished());
	TestEqual(TEXT("the depth body took the hit"), DepthHealth->GetHealth(), 93.0f);
	return true;
}

#endif
