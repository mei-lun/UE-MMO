// M5-032: the explosion motion policy tests. Pins the single detonation at
// the first blocking contact (one blast flag raised before any callback,
// re-entry and second bodies never repeat), the distance falloff (center 1,
// edge EdgeScale, outside the radius nothing - including a depth-axis
// target), the world-static occlusion line (a walled target takes nothing
// while an open one pays), the filter face (friendlies, the shot's own
// source and a dead target never pay; the kill and its presentation happen
// exactly once), the life-loss-only-through-HitApplication ledger structure,
// the explosion+pierce illegal combination and the fail-closed Begin probes
// with legal controls, and the context filter bits deciding block-vs-pass.
//
// Health-assertion convention: the cases assert SURVIVING HEALTH (pool minus
// the blast damage), each with a +/-3 slack band covering the engine
// contact-offset slack on the blast center position (~2.2cm, M5-029) and the
// resolver's round-to-nearest.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/System/UnifiedHitApplier.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/LinearProjectilePolicy.h"
#include "../Projectiles/Policies/ExplosionPolicy.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_032
{
	// Scene placement: a remote base (031's suite occupies the 160000/80000
	// neighborhood); X is the flight axis, Y the depth axis (one lane band per
	// sub-scene, no geometric overlap), Z height.
	const FVector M5_032_SceneBase(200000.0, 120000.0, 800.0);

	// Flight speed for every case: 6000 cm/s.
	const float M5_032_SpeedCmS = 6000.0f;

	// The one shot id of this test suite (pellet index and target distinguish keys).
	constexpr FShotId M5_032_ShotId = 720;

	/** A fully resolved per-hit profile (pure value; the policy never reads catalogs). */
	static FDamageProfile M5_032_MakeProfile(float BaseDamage, const TCHAR* Id)
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = Id;
		Profile.BaseDamage = BaseDamage;
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
	static UWorld* M5_032_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_032_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_032 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_032 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_032_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_032_WorldScope()
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
	 * source entity. The shooter carries a pawn-type collision body plus
	 * health/combat so the blast's self filter is observable: when the
	 * shooter's offset puts it inside a blast radius, the overlap finds it
	 * and the classification (Self) must keep it whole.
	 */
	struct FM5_032_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FTestTargetIdentity Identity;
		FCombatEpoch Epoch = 1;
		FEntityId ShooterId = InvalidCombatEntityId;
		AActor* Shooter = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& ShooterOffset = FVector(-2000.0, 0.0, 0.0))
		{
			FActorSpawnParameters Params;
			Shooter = World.SpawnActor<AActor>(AActor::StaticClass(), M5_032_SceneBase + ShooterOffset, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("a shooter actor spawned"), Shooter))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Shooter, TEXT("M5_032_ShooterBox"));
			Shooter->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(FVector(30.0, 30.0, 90.0));
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
			Body->RegisterComponent();
			Body->SetWorldLocation(M5_032_SceneBase + ShooterOffset);
			UHealthComponent* Health = NewObject<UHealthComponent>(Shooter, TEXT("M5_032_ShooterHealth"));
			Health->RegisterComponent();
			UCombatComponent* Combat = NewObject<UCombatComponent>(Shooter, TEXT("M5_032_ShooterCombat"));
			Combat->RegisterComponent();
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
			return Id != nullptr ? *Id : InvalidCombatTargetId;
		}
	};

	/**
	 * Spawns one query-only box body actor at Center. ObjectType picks the
	 * collision object type: registered bodies are pawns, walls are
	 * world-static (the blast occlusion line only sees static/dynamic).
	 */
	static AActor* M5_032_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& Extent,
		const TCHAR* BodyName, ECollisionChannel ObjectType)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, BodyName);
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

	/** Spawns a registered target (box body + health + combat) with its own faction. */
	static AActor* M5_032_SpawnRegisteredTarget(FAutomationTestBase& Test, UWorld& World, FM5_032_Scene& Scene,
		const FVector& Center, const TCHAR* Faction)
	{
		AActor* Actor = M5_032_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), TEXT("M5_032_Box"), ECC_Pawn);
		if (!Test.TestNotNull(TEXT("a target actor spawned"), Actor))
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_032_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_032_Combat"));
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

	/**
	 * Spawns a registered target with TWO box bodies (the root box plus a
	 * second one attached 150cm behind it along -X): one entity, two bodies
	 * inside the blast radius - the blast must submit exactly once.
	 */
	static AActor* M5_032_SpawnMultiBodyTarget(FAutomationTestBase& Test, UWorld& World, FM5_032_Scene& Scene,
		const FVector& Center)
	{
		AActor* Actor = M5_032_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), TEXT("M5_032_BoxA"), ECC_Pawn);
		if (!Test.TestNotNull(TEXT("a multi-body target actor spawned"), Actor))
		{
			return nullptr;
		}
		UBoxComponent* Second = NewObject<UBoxComponent>(Actor, TEXT("M5_032_BoxB"));
		Second->SetupAttachment(Actor->GetRootComponent());
		Second->SetMobility(EComponentMobility::Movable);
		Second->SetBoxExtent(FVector(30.0, 30.0, 90.0));
		Second->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Second->SetCollisionObjectType(ECC_Pawn);
		Second->SetCollisionResponseToAllChannels(ECR_Ignore);
		Second->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Second->SetRelativeLocation(FVector(-150.0, 0.0, 0.0));
		Second->RegisterComponent();
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_032_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_032_Combat"));
		Combat->RegisterComponent();
		FCombatEntityMetadata Metadata;
		Metadata.Faction = TEXT("Enemy");
		Metadata.Category = TEXT("target");
		const FEntityId Id = Scene.Registry.RegisterEntity(Actor, Metadata, Scene.Epoch);
		if (!Test.TestTrue(TEXT("the multi-body target registered with a valid id"), Id != InvalidCombatEntityId))
		{
			return nullptr;
		}
		Scene.Identity.Record(*Actor, Id);
		return Actor;
	}

	/** Spawns an unregistered world-static box (a wall: blast blocker + occluder). */
	static AActor* M5_032_SpawnWall(UWorld& World, const FVector& Center, const FVector& Extent)
	{
		return M5_032_SpawnBoxActor(World, Center, Extent, TEXT("M5_032_Wall"), ECC_WorldStatic);
	}

	/** Spawns a pellet actor with its provenance stamped (straight flight, gravity scale 0). */
	static ACombatProjectile* M5_032_SpawnProjectile(FAutomationTestBase& Test, UWorld& World,
		const FVector& Origin, FEntityId SourceId, FPelletIndex PelletIndex)
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
		Actor->Context.ShotId = M5_032_ShotId;
		Actor->Context.PelletIndex = PelletIndex;
		Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
		Actor->Context.ProjectileId = TEXT("bullet_explosion_test");
		Actor->LaunchSpeedCmS = M5_032_SpeedCmS;
		Actor->MotionGravityScale = 0.0f;
		return Actor;
	}

	/** The hit context for one scene (filter bits and pierce budget from the caller). */
	static FProjectileHitContext M5_032_MakeHitContext(FM5_032_Scene& Scene, int32 PierceCount,
		bool bFriendliesBlock, bool bUnregisteredBlock, float BaseDamage)
	{
		FProjectileHitContext Context;
		Context.Registry = &Scene.Registry;
		Context.Ledger = &Scene.Ledger;
		Context.Identity = &Scene.Identity;
		Context.AttackProfile = M5_032_MakeProfile(BaseDamage, TEXT("m5_032_blast_round"));
		Context.PierceCount = PierceCount;
		Context.bFriendliesBlockShot = bFriendliesBlock;
		Context.bUnregisteredActorsBlockShot = bUnregisteredBlock;
		Context.SourceActor = Scene.Shooter;
		return Context;
	}

	/** Configures and begins one blast pellet; a refusal fails the case. */
	static bool M5_032_Begin(FAutomationTestBase& Test, FExplosionProjectilePolicy& Policy, ACombatProjectile& Actor,
		const FVector& Direction, const FProjectileHitContext& Context, float Radius, float EdgeScale, const TCHAR* What)
	{
		FExplosionPolicyConfig Config;
		Config.ExplosionRadiusCm = Radius;
		Config.EdgeScale = EdgeScale;
		return Test.TestTrue(FString::Printf(TEXT("%s: the policy began"), What),
			Policy.Configure(Config) && Policy.Begin(Actor, Direction, Context));
	}

	/** The five-tuple key of one hit of this suite. */
	static FCombatEventKey M5_032_MakeKey(const FM5_032_Scene& Scene, FEntityId TargetId, FPelletIndex PelletIndex)
	{
		FCombatEventKey Key;
		Key.Epoch = 1;
		Key.EntityId = Scene.ShooterId;
		Key.ShotId = M5_032_ShotId;
		Key.PelletIndex = PelletIndex;
		Key.TargetId = TargetId;
		return Key;
	}

	/** True when the health sits inside [Low, High] (surviving health with contact-offset slack). */
	static bool M5_032_HealthInRange(const AActor* Target, float Low, float High)
	{
		const UHealthComponent* Health = Target->FindComponentByClass<UHealthComponent>();
		return Health != nullptr && Health->GetHealth() >= Low && Health->GetHealth() <= High;
	}

	/** The actor's health through the health component; -1 when absent. */
	static float M5_032_HealthOf(const AActor* Target)
	{
		const UHealthComponent* Health = Target != nullptr ? Target->FindComponentByClass<UHealthComponent>() : nullptr;
		return Health != nullptr ? Health->GetHealth() : -1.0f;
	}
}

using namespace UE::UEMMO::Tasks::M5_032;

// ---------------------------------------------------------------------------
// CenterEdgeAndOutsideFalloff
// ---------------------------------------------------------------------------

// The falloff face: the wall contact point is the blast center; a target
// ~100cm away pays near-full scale, ~500cm the midpoint scale, ~950cm the
// near-edge scale, a depth-axis target at ~806cm slant pays its own band,
// and a target beyond the radius takes nothing. Surviving health asserted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032CenterEdgeAndOutsideFalloff,
	"UEMMO.Tasks.M5_032.CenterEdgeAndOutsideFalloff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032CenterEdgeAndOutsideFalloff::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// Lane A (Y=0): the pellet flies into a hostile body and the blast
	// happens at its surface (blast center ~962..972 by the contact offset).
	// Every falloff target stands OFF the flight line (the flight line owns
	// Y=0 up to the detonation point - a target on it would be hit first,
	// the 21-10-55 red-green diagnosis); the slant distances carry the scale.
	AActor* Detonator = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + FVector(1000.0, 0.0, 800.0), TEXT("Enemy"));
	AActor* TargetClose = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + FVector(1000.0, 150.0, 800.0), TEXT("Enemy"));
	AActor* TargetMid = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + FVector(1000.0, 500.0, 800.0), TEXT("Enemy"));
	AActor* TargetEdge = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + FVector(1000.0, 950.0, 800.0), TEXT("Enemy"));
	AActor* TargetOutside = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + FVector(1000.0, 1300.0, 800.0), TEXT("Enemy"));
	// The depth-axis target: ~875cm slant from the blast center.
	AActor* TargetDeep = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + FVector(610.0, 800.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the detonator spawned"), Detonator) || !TestNotNull(TEXT("the close target spawned"), TargetClose)
		|| !TestNotNull(TEXT("the mid target spawned"), TargetMid) || !TestNotNull(TEXT("the edge target spawned"), TargetEdge)
		|| !TestNotNull(TEXT("the outside target spawned"), TargetOutside) || !TestNotNull(TEXT("the depth target spawned"), TargetDeep))
	{
		return false;
	}

	ACombatProjectile* Pellet = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pellet spawned"), Pellet))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy;
	if (!M5_032_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f),
		1000.0f, 0.25f, TEXT("the falloff case")))
	{
		return false;
	}
	Policy.AdvanceMotion(0.25f); // 1500cm: hits the detonator body and blasts

	TestTrue(TEXT("the pellet finished at the blast"), Policy.IsFinished());
	TestEqual(TEXT("exactly one detonation"), Policy.GetNumDetonations(), 1);
	// The body rests one body-radius off the detonator's surface (30 extent
	// + 8 radius = 38, the Chaos contact offset absorbed).
	const float PelletSurfaceDistance = FVector::Dist(Pellet->GetActorLocation(), Detonator->GetActorLocation());
	TestTrue(TEXT("the pellet stopped at the detonator's surface"),
		PelletSurfaceDistance > 33.0f && PelletSurfaceDistance < 45.0f);

	// The detonator: the contact point is at its surface, near-full scale.
	TestTrue(TEXT("the detonator paid the near-center scale"), M5_032_HealthInRange(Detonator, 0.0f, 8.0f));
	// Close (~153cm slant, t~0.153, scale ~0.885): ~88.5 damage -> ~11.5 health.
	TestTrue(TEXT("the close target paid the near-center band"), M5_032_HealthInRange(TargetClose, 8.0f, 15.0f));
	// Mid (~501cm, t~0.501, scale ~0.624): ~62.4 damage -> ~37.6 health.
	TestTrue(TEXT("the mid target paid the midpoint scale"), M5_032_HealthInRange(TargetMid, 34.0f, 41.0f));
	// Edge (~950cm, t~0.95, scale ~0.287): ~28.7 damage -> ~71.3 health.
	TestTrue(TEXT("the edge target paid the near-edge scale"), M5_032_HealthInRange(TargetEdge, 68.0f, 74.0f));
	// Outside the radius (~1300cm): nothing.
	TestEqual(TEXT("the outside target untouched"), M5_032_HealthOf(TargetOutside), 100.0f);
	// Depth axis (~875cm slant, t~0.875, scale ~0.344): ~34.4 damage -> ~65.6 health.
	TestTrue(TEXT("the depth target paid its slant-distance scale"), M5_032_HealthInRange(TargetDeep, 62.0f, 69.0f));

	// The ledger structure: one event per damaged target (the detonator and
	// four in-radius targets), the outside target never keyed.
	TestEqual(TEXT("five hostile submissions reported"), Policy.GetNumExplosionSubmissions(), 5);
	TestEqual(TEXT("exactly five ledger events"), Scene.Ledger.GetNumRecordedEvents(), 5);
	TestTrue(TEXT("the close target keyed once"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetClose), 0)));
	TestTrue(TEXT("the depth target keyed once"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetDeep), 0)));
	TestFalse(TEXT("the outside target never keyed"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetOutside), 0)));
	return true;
}

// ---------------------------------------------------------------------------
// WallOcclusionBlocksBlast
// ---------------------------------------------------------------------------

// The occlusion face: the blast center -> target-center line is tested
// against world-static blockers. A target behind an intermediate wall takes
// nothing while its open twin (same band, off the shadow) pays - the legal
// control proving the occlusion, not the distance, decided.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032WallOcclusionBlocksBlast,
	"UEMMO.Tasks.M5_032.WallOcclusionBlocksBlast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032WallOcclusionBlocksBlast::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// Lane B (Y=4000): a hostile target body detonates the pellet at its
	// surface (blast center 962..972); an intermediate world-static wall
	// shadows one target.
	const FVector Lane = FVector(0.0, 4000.0, 0.0);
	AActor* Detonator = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(1000.0, 0.0, 800.0), TEXT("Enemy"));
	AActor* OccluderWall = M5_032_SpawnWall(*World, M5_032_SceneBase + Lane + FVector(1050.0, 0.0, 800.0), FVector(10.0, 100.0, 400.0));
	AActor* TargetShadowed = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(1100.0, 0.0, 800.0), TEXT("Enemy"));
	AActor* TargetOpen = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(1150.0, 600.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the detonator target spawned"), Detonator) || !TestNotNull(TEXT("the occluder wall spawned"), OccluderWall)
		|| !TestNotNull(TEXT("the shadowed target spawned"), TargetShadowed) || !TestNotNull(TEXT("the open target spawned"), TargetOpen))
	{
		return false;
	}

	ACombatProjectile* Pellet = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pellet spawned"), Pellet))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy;
	if (!M5_032_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f),
		1000.0f, 0.25f, TEXT("the occlusion case")))
	{
		return false;
	}
	Policy.AdvanceMotion(0.25f); // 1500cm: hits the detonator and blasts

	TestTrue(TEXT("the pellet finished at the blast"), Policy.IsFinished());
	TestEqual(TEXT("exactly one detonation"), Policy.GetNumDetonations(), 1);
	// The shadowed target: behind the wall, nothing.
	TestEqual(TEXT("the shadowed target untouched"), M5_032_HealthOf(TargetShadowed), 100.0f);
	// The open twin: same blast, no wall between - it paid (the control).
	TestTrue(TEXT("the open target paid (the occlusion control)"),
		M5_032_HealthInRange(TargetOpen, 40.0f, 54.0f));
	// The detonator itself: the contact point is at its surface, near-full scale.
	TestTrue(TEXT("the detonator paid the near-center scale"),
		M5_032_HealthInRange(Detonator, 0.0f, 8.0f));
	// The ledger: exactly the detonator and the open target.
	TestEqual(TEXT("exactly two ledger events"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestFalse(TEXT("the shadowed target never keyed"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetShadowed), 0)));
	return true;
}

// ---------------------------------------------------------------------------
// FriendlySelfDeadFiltered
// ---------------------------------------------------------------------------

// The filter face: same-faction bodies, the shot's own source and a dead
// target never pay; the blast still reaches an open hostile (the control).
// The dead target was killed through the unified entry BEFORE the blast -
// the blast's own submission for it is refused and nothing is recorded
// twice: kill and presentation each exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032FriendlySelfDeadFiltered,
	"UEMMO.Tasks.M5_032.FriendlySelfDeadFiltered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032FriendlySelfDeadFiltered::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	// Lane C (Y=8000): a wall face detonates; the shooter sits 950cm behind
	// the blast center - inside the radius, protected by the self filter.
	const FVector Lane = FVector(0.0, 8000.0, 0.0);
	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World, Lane + FVector(2000.0, 0.0, 0.0)))
	{
		return false;
	}

	AActor* Wall = M5_032_SpawnWall(*World, M5_032_SceneBase + Lane + FVector(1100.0, 0.0, 800.0), FVector(50.0, 400.0, 400.0));
	AActor* TargetFriendly = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(950.0, 0.0, 800.0), TEXT("Player"));
	AActor* TargetDead = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(650.0, 0.0, 800.0), TEXT("Enemy"));
	AActor* TargetOpen = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(850.0, -500.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the blast wall spawned"), Wall) || !TestNotNull(TEXT("the friendly target spawned"), TargetFriendly)
		|| !TestNotNull(TEXT("the doomed target spawned"), TargetDead) || !TestNotNull(TEXT("the open target spawned"), TargetOpen))
	{
		return false;
	}

	// Kill the doomed target through the unified entry before the blast
	// (its own pellet slot, its own key).
	FUnifiedHitRequest KillRequest;
	KillRequest.Epoch = Scene.Epoch;
	KillRequest.AttackerEntityId = Scene.ShooterId;
	KillRequest.ShotId = M5_032_ShotId;
	KillRequest.PelletIndex = 250;
	KillRequest.TargetEntityId = Scene.IdOf(*TargetDead);
	KillRequest.TargetActor = TargetDead;
	KillRequest.Attack = M5_032_MakeProfile(150.0f, TEXT("m5_032_kill_round"));
	KillRequest.AttackPower = 0.0f;
	KillRequest.HitLocation = TargetDead->GetActorLocation();
	if (UCombatComponent* TargetCombat = TargetDead->FindComponentByClass<UCombatComponent>())
	{
		KillRequest.TargetReaction = TargetCombat->GetTargetReactionPolicy();
	}
	const FUnifiedHitOutcome KillOutcome = ApplyUnifiedHit(KillRequest, Scene.Ledger);
	TestTrue(TEXT("the pre-blast kill applied"), KillOutcome.WasApplied());
	TestTrue(TEXT("the pre-blast kill killed the target"), KillOutcome.bTargetDied);
	TestEqual(TEXT("the doomed target is dead"), M5_032_HealthOf(TargetDead), 0.0f);

	ACombatProjectile* Pellet = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pellet spawned"), Pellet))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy;
	if (!M5_032_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f),
		1000.0f, 0.25f, TEXT("the filter case")))
	{
		return false;
	}
	Policy.AdvanceMotion(0.25f); // 1500cm: the wall face detonates

	TestTrue(TEXT("the pellet finished at the blast"), Policy.IsFinished());
	TestEqual(TEXT("exactly one detonation"), Policy.GetNumDetonations(), 1);
	// The friendly: same faction, never submitted.
	TestEqual(TEXT("the friendly target untouched"), M5_032_HealthOf(TargetFriendly), 100.0f);
	// The shooter (the blast's own source, inside the radius): the self
	// filter, never submitted.
	TestEqual(TEXT("the shooter untouched by its own blast"), M5_032_HealthOf(Scene.Shooter), 100.0f);
	// The dead target: refused, stays dead at exactly 0 - no double kill.
	TestEqual(TEXT("the dead target stays at zero"), M5_032_HealthOf(TargetDead), 0.0f);
	// The control: the open hostile paid (~59.6 damage -> ~40.4 health).
	TestTrue(TEXT("the open hostile paid (the filter control)"),
		M5_032_HealthInRange(TargetOpen, 37.0f, 44.0f));
	// The ledger: the pre-kill + the open hostile only - the friendly, the
	// shooter and the dead target left nothing behind.
	TestEqual(TEXT("exactly two ledger events"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestFalse(TEXT("the friendly never keyed"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetFriendly), 0)));
	TestTrue(TEXT("the dead target has only its pre-kill key"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetDead), 250))
		&& !Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetDead), 0)));
	return true;
}

// ---------------------------------------------------------------------------
// SingleDetonationNoRepeat
// ---------------------------------------------------------------------------

// The one-blast face: the flag rises before any callback; a re-entered
// AdvanceMotion after the blast is a no-op (no second submission, no body
// movement), a multi-body target inside the radius is submitted exactly
// once, and two pellets of one shot stay independent (each blast its own).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032SingleDetonationNoRepeat,
	"UEMMO.Tasks.M5_032.SingleDetonationNoRepeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032SingleDetonationNoRepeat::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// Lane D (Y=12000): wall blast onto a multi-body target.
	const FVector LaneD = FVector(0.0, 12000.0, 0.0);
	AActor* WallD = M5_032_SpawnWall(*World, M5_032_SceneBase + LaneD + FVector(1100.0, 0.0, 800.0), FVector(50.0, 400.0, 400.0));
	AActor* TargetMulti = M5_032_SpawnMultiBodyTarget(*this, *World, Scene, M5_032_SceneBase + LaneD + FVector(950.0, 0.0, 800.0));
	if (!TestNotNull(TEXT("the lane D wall spawned"), WallD) || !TestNotNull(TEXT("the multi-body target spawned"), TargetMulti))
	{
		return false;
	}
	ACombatProjectile* Pellet1 = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + LaneD + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the lane D pellet spawned"), Pellet1))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy1;
	if (!M5_032_Begin(*this, Policy1, *Pellet1, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f),
		1000.0f, 0.25f, TEXT("the one-blast case")))
	{
		return false;
	}
	Policy1.AdvanceMotion(0.25f); // 1500cm: the wall face detonates
	TestTrue(TEXT("the pellet finished at the blast"), Policy1.IsFinished());
	TestEqual(TEXT("exactly one detonation"), Policy1.GetNumDetonations(), 1);
	// The multi-body target: two bodies inside the radius, one entity - one submission.
	TestEqual(TEXT("exactly one explosion submission for the multi-body target"), Policy1.GetNumExplosionSubmissions(), 1);
	TestEqual(TEXT("exactly one ledger event"), Scene.Ledger.GetNumRecordedEvents(), 1);
	const float MultiHealthAfterBlast = M5_032_HealthOf(TargetMulti);
	TestTrue(TEXT("the multi-body target paid once (near-center scale)"),
		MultiHealthAfterBlast > 4.5f && MultiHealthAfterBlast < 10.5f);
	const FVector BodyLocationAfterBlast = Pellet1->GetActorLocation();

	// The re-entry: a second advance after the blast changes nothing.
	Policy1.AdvanceMotion(1.0f); // a big step - the finished pellet ignores it
	TestTrue(TEXT("the re-entered advance stays finished"), Policy1.IsFinished());
	TestEqual(TEXT("the re-entered advance adds no detonation"), Policy1.GetNumDetonations(), 1);
	TestEqual(TEXT("the re-entered advance adds no submission"), Policy1.GetNumExplosionSubmissions(), 1);
	TestEqual(TEXT("the re-entered advance adds no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 1);
	TestEqual(TEXT("the re-entered advance leaves the health still"), M5_032_HealthOf(TargetMulti), MultiHealthAfterBlast);
	TestTrue(TEXT("the re-entered advance leaves the body unmoved"),
		Pellet1->GetActorLocation().Equals(BodyLocationAfterBlast, 0.01f));

	// Lane E (Y=16000): the shot's second pellet is an independent blast.
	const FVector LaneE = FVector(0.0, 16000.0, 0.0);
	AActor* WallE = M5_032_SpawnWall(*World, M5_032_SceneBase + LaneE + FVector(1100.0, 0.0, 800.0), FVector(50.0, 400.0, 400.0));
	AActor* TargetSecond = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + LaneE + FVector(950.0, 0.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the lane E wall spawned"), WallE) || !TestNotNull(TEXT("the lane E target spawned"), TargetSecond))
	{
		return false;
	}
	ACombatProjectile* Pellet2 = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + LaneE + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("the lane E pellet spawned"), Pellet2))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy2;
	if (!M5_032_Begin(*this, Policy2, *Pellet2, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f),
		1000.0f, 0.25f, TEXT("the second pellet")))
	{
		return false;
	}
	Policy2.AdvanceMotion(0.25f);
	TestEqual(TEXT("the second pellet blasted once"), Policy2.GetNumDetonations(), 1);
	TestEqual(TEXT("the second pellet's own submission"), Policy2.GetNumExplosionSubmissions(), 1);
	TestEqual(TEXT("the two pellets share no accounting"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestTrue(TEXT("the second target keyed under the second pellet"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetSecond), 1)));
	return true;
}

// ---------------------------------------------------------------------------
// HostileContactDetonatesAndPaysOnce
// ---------------------------------------------------------------------------

// The contact face: a hostile body itself detonates the pellet at its
// surface (no wall needed), the contact target pays near-full scale and the
// blast kills - exactly once: one ledger key, the pool drained once, and the
// neighbor target in the radius band paid its own falloff.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032HostileContactDetonatesAndPaysOnce,
	"UEMMO.Tasks.M5_032.HostileContactDetonatesAndPaysOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032HostileContactDetonatesAndPaysOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// Lane F (Y=20000): the pellet flies into a hostile body (150 base
	// damage) and the blast kills it; a neighbor target pays its band.
	const FVector Lane = FVector(0.0, 20000.0, 0.0);
	AActor* TargetContact = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(1000.0, 0.0, 800.0), TEXT("Enemy"));
	AActor* TargetNeighbor = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(1000.0, 600.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the contact target spawned"), TargetContact) || !TestNotNull(TEXT("the neighbor target spawned"), TargetNeighbor))
	{
		return false;
	}

	ACombatProjectile* Pellet = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pellet spawned"), Pellet))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy;
	if (!M5_032_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 150.0f),
		1000.0f, 0.25f, TEXT("the contact case")))
	{
		return false;
	}
	Policy.AdvanceMotion(0.25f); // 1500cm: the contact body detonates at its surface

	TestTrue(TEXT("the pellet finished at the blast"), Policy.IsFinished());
	TestEqual(TEXT("exactly one detonation"), Policy.GetNumDetonations(), 1);
	// The contact target: near-full scale (~146 of 150) - killed exactly once.
	TestEqual(TEXT("the contact target is dead"), M5_032_HealthOf(TargetContact), 0.0f);
	// The neighbor: slant ~601cm, t~0.6, scale ~0.549 -> ~82 of 150 -> ~18 health.
	TestTrue(TEXT("the neighbor paid its band"),
		M5_032_HealthInRange(TargetNeighbor, 12.0f, 24.0f));
	// The ledger structure: one key per damaged target, life loss only through
	// the unified entry.
	TestEqual(TEXT("exactly two ledger events"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestTrue(TEXT("the contact target keyed once"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetContact), 0)));
	TestTrue(TEXT("the neighbor keyed once"),
		Scene.Ledger.HasRecordedEvent(M5_032_MakeKey(Scene, Scene.IdOf(*TargetNeighbor), 0)));
	return true;
}

// ---------------------------------------------------------------------------
// ExplosionPierceRefusedAndFailClosedBegin
// ---------------------------------------------------------------------------

// The refusal face with its legal controls: a pierce budget refuses the bind
// (one blast never shares a body with piercing), a non-positive radius
// refuses (an explosion policy without a radius is a configuration error),
// out-of-range edge scales and non-finite config values refuse at Configure,
// incomplete contexts, degenerate directions and negative pierce budgets
// refuse at Begin, an in-flight reconfigure refuses - and the legal control
// (radius + zero pierce + valid context) binds, flies and detonates,
// proving the refusals are the policy's real faces.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032ExplosionPierceRefusedAndFailClosedBegin,
	"UEMMO.Tasks.M5_032.ExplosionPierceRefusedAndFailClosedBegin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032ExplosionPierceRefusedAndFailClosedBegin::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	const FVector Lane = FVector(0.0, 24000.0, 0.0);
	// The refusal probes never bind, so the probe target's spot is free;
	// it stands far outside the legal control's blast radius (~3000cm) so
	// the legal control submits exactly once.
	AActor* Target = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(3000.0, 0.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the target spawned"), Target))
	{
		return false;
	}

	// Configure's value faces.
	FExplosionProjectilePolicy ConfigProbe;
	FExplosionPolicyConfig BadEdge;
	BadEdge.ExplosionRadiusCm = 500.0f;
	BadEdge.EdgeScale = 1.5f;
	TestFalse(TEXT("an amplified edge scale refuses"), ConfigProbe.Configure(BadEdge));
	BadEdge.EdgeScale = -0.1f;
	TestFalse(TEXT("a negative edge scale refuses"), ConfigProbe.Configure(BadEdge));
	BadEdge.EdgeScale = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("a NaN edge scale refuses"), ConfigProbe.Configure(BadEdge));
	FExplosionPolicyConfig BadRadius;
	BadRadius.ExplosionRadiusCm = -1.0f;
	BadRadius.EdgeScale = 0.5f;
	TestFalse(TEXT("a negative radius refuses"), ConfigProbe.Configure(BadRadius));
	BadRadius.ExplosionRadiusCm = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("a non-finite radius refuses"), ConfigProbe.Configure(BadRadius));

	// The pierce/blast illegal combination: any pierce budget refuses.
	ACombatProjectile* PelletPierce = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pierce probe pellet spawned"), PelletPierce))
	{
		return false;
	}
	FExplosionProjectilePolicy PolicyPierce;
	FExplosionPolicyConfig GoodConfig;
	GoodConfig.ExplosionRadiusCm = 1000.0f;
	GoodConfig.EdgeScale = 0.25f;
	TestTrue(TEXT("the pierce probe configured"), PolicyPierce.Configure(GoodConfig));
	TestFalse(TEXT("a pierce budget refuses the blast bind"),
		PolicyPierce.Begin(*PelletPierce, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 2, true, true, 100.0f)));
	TestTrue(TEXT("a refused bind stays finished"), PolicyPierce.IsFinished());

	// A non-positive radius refuses the bind (Configure keeps it legal so the
	// wiring can reuse one policy object with a later good configuration).
	ACombatProjectile* PelletZero = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 200.0, 800.0), Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("the zero-radius probe pellet spawned"), PelletZero))
	{
		return false;
	}
	FExplosionProjectilePolicy PolicyZero;
	FExplosionPolicyConfig ZeroRadius;
	ZeroRadius.ExplosionRadiusCm = 0.0f;
	ZeroRadius.EdgeScale = 0.25f;
	TestTrue(TEXT("a zero radius configures"), PolicyZero.Configure(ZeroRadius));
	TestFalse(TEXT("a zero radius refuses the blast bind"),
		PolicyZero.Begin(*PelletZero, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f)));
	TestTrue(TEXT("the zero-radius refusal stays finished"), PolicyZero.IsFinished());

	// The fail-closed Begin faces: incomplete contexts.
	ACombatProjectile* PelletProbes = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 400.0, 800.0), Scene.ShooterId, 2);
	if (!TestNotNull(TEXT("the probe pellet spawned"), PelletProbes))
	{
		return false;
	}
	FExplosionProjectilePolicy PolicyProbes;
	TestTrue(TEXT("the probe policy configured"), PolicyProbes.Configure(GoodConfig));
	FProjectileHitContext ContextNoRegistry = M5_032_MakeHitContext(Scene, 0, true, true, 100.0f);
	ContextNoRegistry.Registry = nullptr;
	TestFalse(TEXT("a null registry refuses"),
		PolicyProbes.Begin(*PelletProbes, FVector(1.0, 0.0, 0.0), ContextNoRegistry));
	FProjectileHitContext ContextNoLedger = M5_032_MakeHitContext(Scene, 0, true, true, 100.0f);
	ContextNoLedger.Ledger = nullptr;
	TestFalse(TEXT("a null ledger refuses"),
		PolicyProbes.Begin(*PelletProbes, FVector(1.0, 0.0, 0.0), ContextNoLedger));
	FProjectileHitContext ContextNoIdentity = M5_032_MakeHitContext(Scene, 0, true, true, 100.0f);
	ContextNoIdentity.Identity = nullptr;
	TestFalse(TEXT("a null identity refuses"),
		PolicyProbes.Begin(*PelletProbes, FVector(1.0, 0.0, 0.0), ContextNoIdentity));
	// A degenerate direction and a negative pierce budget refuse too.
	TestFalse(TEXT("a zero direction refuses"),
		PolicyProbes.Begin(*PelletProbes, FVector::ZeroVector, M5_032_MakeHitContext(Scene, 0, true, true, 100.0f)));
	TestFalse(TEXT("a negative pierce budget refuses"),
		PolicyProbes.Begin(*PelletProbes, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, -1, true, true, 100.0f)));

	// The legal control: the pierce-free context binds, flies, detonates on
	// the target - and an in-flight reconfigure refuses afterwards.
	ACombatProjectile* PelletLegal = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 600.0, 800.0), Scene.ShooterId, 3);
	AActor* TargetLegal = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(1000.0, 600.0, 800.0), TEXT("Enemy"));
	if (!TestNotNull(TEXT("the legal pellet spawned"), PelletLegal) || !TestNotNull(TEXT("the legal target spawned"), TargetLegal))
	{
		return false;
	}
	FExplosionProjectilePolicy PolicyLegal;
	if (!M5_032_Begin(*this, PolicyLegal, *PelletLegal, FVector(1.0, 0.0, 0.0), M5_032_MakeHitContext(Scene, 0, true, true, 100.0f),
		1000.0f, 0.25f, TEXT("the legal control")))
	{
		return false;
	}
	TestFalse(TEXT("an in-flight reconfigure refuses"), PolicyLegal.Configure(GoodConfig));
	PolicyLegal.AdvanceMotion(0.25f); // 1500cm: the legal target detonates the pellet
	TestTrue(TEXT("the legal control finished at the blast"), PolicyLegal.IsFinished());
	TestEqual(TEXT("the legal control detonated exactly once"), PolicyLegal.GetNumDetonations(), 1);
	TestEqual(TEXT("the legal control submitted once"), PolicyLegal.GetNumExplosionSubmissions(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// PassThroughBitsFollowContextFilter
// ---------------------------------------------------------------------------

// The filter-bit face: with both bits false the pellet flies through a
// friendly body and a wall without detonating (no blast, no damage, the body
// travels the full step); the legal control (both bits true) detonates on
// the friendly block exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_032PassThroughBitsFollowContextFilter,
	"UEMMO.Tasks.M5_032.PassThroughBitsFollowContextFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_032PassThroughBitsFollowContextFilter::RunTest(const FString& Parameters)
{
	UWorld* World = M5_032_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_032_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_032_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// Lane G (Y=28000): bit-false pellet flies through a friendly and a wall.
	const FVector Lane = FVector(0.0, 28000.0, 0.0);
	AActor* TargetFriendly = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(500.0, 0.0, 800.0), TEXT("Player"));
	AActor* Wall = M5_032_SpawnWall(*World, M5_032_SceneBase + Lane + FVector(1100.0, 0.0, 800.0), FVector(50.0, 400.0, 400.0));
	if (!TestNotNull(TEXT("the friendly target spawned"), TargetFriendly) || !TestNotNull(TEXT("the wall spawned"), Wall))
	{
		return false;
	}

	ACombatProjectile* Pellet1 = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 0.0, 800.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pass-through pellet spawned"), Pellet1))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy1;
	if (!M5_032_Begin(*this, Policy1, *Pellet1, FVector(1.0, 0.0, 0.0),
		M5_032_MakeHitContext(Scene, 0, /*bFriendliesBlock*/ false, /*bUnregisteredBlock*/ false, 100.0f),
		1000.0f, 0.25f, TEXT("the pass-through case")))
	{
		return false;
	}
	Policy1.AdvanceMotion(0.25f); // 1500cm: through the friendly (X=500) and the wall (X=1050..1150)
	TestEqual(TEXT("no detonation on the pass-through flight"), Policy1.GetNumDetonations(), 0);
	TestEqual(TEXT("no submission on the pass-through flight"), Policy1.GetNumExplosionSubmissions(), 0);
	TestEqual(TEXT("no ledger event on the pass-through flight"), Scene.Ledger.GetNumRecordedEvents(), 0);
	TestEqual(TEXT("the friendly passed through untouched"), M5_032_HealthOf(TargetFriendly), 100.0f);
	TestTrue(TEXT("the body flew the full step through both blockers"), Pellet1->GetActorLocation().X > 1400.0f);

	// The legal control: both bits true - the friendly blocks and detonates.
	ACombatProjectile* Pellet2 = M5_032_SpawnProjectile(*this, *World, M5_032_SceneBase + Lane + FVector(0.0, 800.0, 800.0), Scene.ShooterId, 1);
	AActor* TargetFriendly2 = M5_032_SpawnRegisteredTarget(*this, *World, Scene, M5_032_SceneBase + Lane + FVector(500.0, 800.0, 800.0), TEXT("Player"));
	if (!TestNotNull(TEXT("the control pellet spawned"), Pellet2) || !TestNotNull(TEXT("the control friendly spawned"), TargetFriendly2))
	{
		return false;
	}
	FExplosionProjectilePolicy Policy2;
	if (!M5_032_Begin(*this, Policy2, *Pellet2, FVector(1.0, 0.0, 0.0),
		M5_032_MakeHitContext(Scene, 0, /*bFriendliesBlock*/ true, /*bUnregisteredBlock*/ true, 100.0f),
		1000.0f, 0.25f, TEXT("the block control")))
	{
		return false;
	}
	Policy2.AdvanceMotion(0.25f);
	TestTrue(TEXT("the control pellet finished at the friendly block"), Policy2.IsFinished());
	TestEqual(TEXT("the control detonated exactly once"), Policy2.GetNumDetonations(), 1);
	// The blast only damages hostiles: the friendly detonator stays whole.
	TestEqual(TEXT("the friendly detonator stays untouched"), M5_032_HealthOf(TargetFriendly2), 100.0f);
	TestEqual(TEXT("no hostile was inside the control blast"), Policy2.GetNumExplosionSubmissions(), 0);
	return true;
}

#endif
