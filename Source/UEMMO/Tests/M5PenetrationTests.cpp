// M5-031: the penetration (bounded pierce) motion policy tests. Pins the
// bounded pierce budget (N extra targets, hits strictly in distance order),
// the world-obstacle always-block face, the per-projectile hit set (an
// already-hit entity is never re-damaged and never consumes a slot - the
// same-actor multi-body and the re-contact faces), two pellets of one shot
// staying independent with the ledger control dedup intact, the friendly /
// category-tag filtering without wasting pierce slots, the explosion+pierce
// illegal-combination refusal and the fail-closed Begin probes, and the
// end-set cleanup with the no-residual-callback invalidation face.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/LinearProjectilePolicy.h"
#include "../Projectiles/Policies/PenetrationPolicy.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_031
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the flight axis, Y the depth axis (one lane band per
	// sub-scene, no geometric overlap), Z height.
	const FVector M5_031_SceneBase(160000.0, 80000.0, 800.0);

	// Flight speed for every case: 6000 cm/s.
	const float M5_031_SpeedCmS = 6000.0f;

	// The one shot id of this test suite (pellet index and target distinguish keys).
	constexpr FShotId M5_031_ShotId = 700;

	/** A fully resolved per-hit profile (pure value; the policy never reads catalogs). */
	static FDamageProfile M5_031_MakeProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("penetration_test_round");
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
	static UWorld* M5_031_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_031_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_031 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_031 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_031_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_031_WorldScope()
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
	 * source entity (body-less shooter behind the flight lines).
	 */
	struct FM5_031_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FTestTargetIdentity Identity;
		FCombatEpoch Epoch = 1;
		FEntityId ShooterId = InvalidCombatEntityId;
		AActor* Shooter = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			FActorSpawnParameters Params;
			Shooter = World.SpawnActor<AActor>(AActor::StaticClass(), M5_031_SceneBase + FVector(-500.0, 0.0, 0.0), FRotator::ZeroRotator, Params);
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

	/** Spawns one query-only box body actor at Center (root component). */
	static AActor* M5_031_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& Extent, const TCHAR* BodyName)
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
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		// The projectile body blocks this object type; the blocking face needs
		// the reciprocal response (either side ignoring kills the block pair).
		Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Body->RegisterComponent();
		Body->SetWorldLocation(Center);
		return Actor;
	}

	/** Spawns a registered target (box body + health + combat) with its own category tag. */
	static AActor* M5_031_SpawnRegisteredTarget(FAutomationTestBase& Test, UWorld& World, FM5_031_Scene& Scene,
		const FVector& Center, const TCHAR* Faction, const TCHAR* Category)
	{
		AActor* Actor = M5_031_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), TEXT("M5_031_Box"));
		if (!Test.TestNotNull(TEXT("a target actor spawned"), Actor))
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_031_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_031_Combat"));
		Combat->RegisterComponent();
		FCombatEntityMetadata Metadata;
		Metadata.Faction = Faction;
		Metadata.Category = Category;
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
	 * second one attached 150cm behind it along -X): one entity, two contact
	 * surfaces, one compound bounding box.
	 */
	static AActor* M5_031_SpawnMultiBodyTarget(FAutomationTestBase& Test, UWorld& World, FM5_031_Scene& Scene,
		const FVector& Center)
	{
		AActor* Actor = M5_031_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), TEXT("M5_031_BoxA"));
		if (!Test.TestNotNull(TEXT("a multi-body target actor spawned"), Actor))
		{
			return nullptr;
		}
		UBoxComponent* Second = NewObject<UBoxComponent>(Actor, TEXT("M5_031_BoxB"));
		Second->SetupAttachment(Actor->GetRootComponent());
		Second->SetMobility(EComponentMobility::Movable);
		Second->SetBoxExtent(FVector(30.0, 30.0, 90.0));
		Second->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Second->SetCollisionObjectType(ECC_Pawn);
		Second->SetCollisionResponseToAllChannels(ECR_Ignore);
		Second->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Second->SetRelativeLocation(FVector(-150.0, 0.0, 0.0));
		Second->RegisterComponent();
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_031_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_031_Combat"));
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

	/** Spawns an unregistered box (a wall: no identity record, never a target). */
	static AActor* M5_031_SpawnWall(UWorld& World, const FVector& Center, const FVector& Extent)
	{
		return M5_031_SpawnBoxActor(World, Center, Extent, TEXT("M5_031_Wall"));
	}

	/** Spawns a pellet actor with its provenance stamped (straight flight, gravity scale 0). */
	static ACombatProjectile* M5_031_SpawnProjectile(FAutomationTestBase& Test, UWorld& World,
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
		Actor->Context.ShotId = M5_031_ShotId;
		Actor->Context.PelletIndex = PelletIndex;
		Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
		Actor->Context.ProjectileId = TEXT("bullet_penetration_test");
		Actor->LaunchSpeedCmS = M5_031_SpeedCmS;
		Actor->MotionGravityScale = 0.0f;
		return Actor;
	}

	/** The default hit context for one scene (filter bits on, the pierce budget from the caller). */
	static FProjectileHitContext M5_031_MakeHitContext(FM5_031_Scene& Scene, int32 PierceCount)
	{
		FProjectileHitContext Context;
		Context.Registry = &Scene.Registry;
		Context.Ledger = &Scene.Ledger;
		Context.Identity = &Scene.Identity;
		Context.AttackProfile = M5_031_MakeProfile();
		Context.PierceCount = PierceCount;
		Context.bFriendliesBlockShot = true;
		Context.bUnregisteredActorsBlockShot = true;
		Context.SourceActor = Scene.Shooter;
		return Context;
	}

	/** Configures and begins one penetration pellet; a refusal fails the case. */
	static bool M5_031_Begin(FAutomationTestBase& Test, FPenetrationProjectilePolicy& Policy, ACombatProjectile& Actor,
		const FVector& Direction, const FProjectileHitContext& Context, const TCHAR* What)
	{
		FPenetrationPolicyConfig Config;
		return Test.TestTrue(FString::Printf(TEXT("%s: the policy began"), What),
			Policy.Configure(Config) && Policy.Begin(Actor, Direction, Context));
	}

	/** The five-tuple key of one hit of this suite. */
	static FCombatEventKey M5_031_MakeKey(const FM5_031_Scene& Scene, FEntityId TargetId, FPelletIndex PelletIndex)
	{
		FCombatEventKey Key;
		Key.Epoch = 1;
		Key.EntityId = Scene.ShooterId;
		Key.ShotId = M5_031_ShotId;
		Key.PelletIndex = PelletIndex;
		Key.TargetId = TargetId;
		return Key;
	}

	/** True when the pellet body stopped at a target's surface (center distance within tolerance). */
	static bool M5_031_StoppedAtSurface(const ACombatProjectile& Pellet, const AActor& Target)
	{
		const float CenterDistance = FVector::Dist(Pellet.GetActorLocation(), Target.GetActorLocation());
		return CenterDistance > 40.0f && CenterDistance < 50.0f; // 30 extent + 15 radius = 45, Chaos contact offset absorbed
	}
}

using namespace UE::UEMMO::Tasks::M5_031;

// ---------------------------------------------------------------------------
// PierceBudgetAndDistanceOrder
// ---------------------------------------------------------------------------

// The pierce budget face: N=0 wounds exactly one target; N=2 wounds at most
// three; hits are processed strictly in distance order (per tick only the
// nearest unreached target is damaged, and one long advance pierces several
// targets nearest-first through the segment loop).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031PierceBudgetAndDistanceOrder,
	"UEMMO.Tasks.M5_031.PierceBudgetAndDistanceOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031PierceBudgetAndDistanceOrder::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// --- Sub-scene A (lane Y=0): N=0 wounds exactly one target. ---
	AActor* TargetA1 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetB1 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + FVector(2000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane A targets spawned"), TargetA1) || !TestNotNull(TEXT("lane A second target spawned"), TargetB1))
	{
		return false;
	}
	ACombatProjectile* Pellet1 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + FVector(0.0, 0.0, 0.0), Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("lane A pellet spawned"), Pellet1))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy1;
	if (!M5_031_Begin(*this, Policy1, *Pellet1, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 0), TEXT("lane A")))
	{
		return false;
	}
	Policy1.AdvanceMotion(0.25f); // 1500cm: reaches target A1, N=0 terminates there
	TestTrue(TEXT("N=0: the pellet finished at the first target"), Policy1.IsFinished());
	TestEqual(TEXT("N=0: exactly one pierce slot consumed"), Policy1.GetNumPiercedHits(), 1);
	TestEqual(TEXT("N=0: exactly one ledger event"), Scene.Ledger.GetNumRecordedEvents(), 1);
	TestTrue(TEXT("N=0: the first target keyed in the ledger"),
		Scene.Ledger.HasRecordedEvent(M5_031_MakeKey(Scene, Scene.IdOf(*TargetA1), 0)));
	TestEqual(TEXT("N=0: the first target damaged once"), TargetA1->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("N=0: the second target untouched"), TargetB1->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestTrue(TEXT("N=0: the body stopped at the first target's surface"), M5_031_StoppedAtSurface(*Pellet1, *TargetA1));

	// --- Sub-scene B (lane Y=3000): N=2 pierces three targets in ONE advance. ---
	const FVector LaneB = FVector(0.0, 3000.0, 0.0);
	AActor* TargetA2 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetB2 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(2000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetC2 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(3000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetD2 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(4000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane B targets spawned"), TargetA2) || !TestNotNull(TEXT("lane B fourth target spawned"), TargetD2))
	{
		return false;
	}
	ACombatProjectile* Pellet2 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneB, Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("lane B pellet spawned"), Pellet2))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy2;
	if (!M5_031_Begin(*this, Policy2, *Pellet2, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2), TEXT("lane B")))
	{
		return false;
	}
	Policy2.AdvanceMotion(0.6f); // 3600cm in one advance: pierces A2, B2, terminates at C2
	TestTrue(TEXT("N=2: the pellet finished at the third target"), Policy2.IsFinished());
	TestEqual(TEXT("N=2: exactly three pierce slots consumed"), Policy2.GetNumPiercedHits(), 3);
	TestEqual(TEXT("N=2: exactly three ledger events"), Scene.Ledger.GetNumRecordedEvents(), 4); // 1 (lane A) + 3
	TestEqual(TEXT("N=2: first pierced target damaged once"), TargetA2->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("N=2: second pierced target damaged once"), TargetB2->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("N=2: third pierced target damaged once"), TargetC2->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("N=2: the fourth target untouched (budget exhausted)"), TargetD2->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestTrue(TEXT("N=2: the body stopped at the third target's surface"), M5_031_StoppedAtSurface(*Pellet2, *TargetC2));

	// --- Sub-scene C (lane Y=6000): N=2 across three ticks, strictly nearest-first. ---
	const FVector LaneC = FVector(0.0, 6000.0, 0.0);
	AActor* TargetA3 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneC + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetB3 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneC + FVector(2000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetC3 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneC + FVector(3000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetD3 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneC + FVector(4000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane C targets spawned"), TargetA3) || !TestNotNull(TEXT("lane C fourth target spawned"), TargetD3))
	{
		return false;
	}
	ACombatProjectile* Pellet3 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneC, Scene.ShooterId, 2);
	if (!TestNotNull(TEXT("lane C pellet spawned"), Pellet3))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy3;
	if (!M5_031_Begin(*this, Policy3, *Pellet3, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2), TEXT("lane C")))
	{
		return false;
	}
	Policy3.AdvanceMotion(0.2f); // 1200cm: only the nearest target (A3) is damaged
	TestEqual(TEXT("tick 1: the nearest target damaged"), TargetA3->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("tick 1: the farther targets untouched (distance order)"), TargetB3->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	Policy3.AdvanceMotion(0.2f); // reaches B3 only
	TestEqual(TEXT("tick 2: the second target damaged in order"), TargetB3->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("tick 2: the third target still untouched"), TargetC3->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	Policy3.AdvanceMotion(0.2f); // reaches C3: the budget (first + 2 extra) is exhausted there
	TestTrue(TEXT("tick 3: the pellet finished at the third target"), Policy3.IsFinished());
	TestEqual(TEXT("tick 3: the third target damaged"), TargetC3->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("tick 3: the fourth target untouched"), TargetD3->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestEqual(TEXT("tick 3: the pierce budget accounting"), Policy3.GetNumPiercedHits(), 3);
	TestEqual(TEXT("tick 3: the ledger events of lane C"), Scene.Ledger.GetNumRecordedEvents(), 7); // 1 + 3 + 3
	return true;
}

// ---------------------------------------------------------------------------
// WorldObstacleAlwaysBlocks
// ---------------------------------------------------------------------------

// The world face: a wall (unregistered body) always blocks a piercing pellet
// - the pellet terminates there with no submission and no pierce slot
// consumed, and the targets behind the wall stay untouched. A context that
// would let unregistered bodies pass is refused at Begin (an illegal
// combination for a piercing pellet).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031WorldObstacleAlwaysBlocks,
	"UEMMO.Tasks.M5_031.WorldObstacleAlwaysBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031WorldObstacleAlwaysBlocks::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	const FVector Lane = FVector(0.0, 9000.0, 0.0);
	AActor* Wall = M5_031_SpawnWall(*World, M5_031_SceneBase + Lane + FVector(800.0, 0.0, 0.0), FVector(30.0, 200.0, 200.0));
	AActor* TargetA = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetB = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(2000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("the wall spawned"), Wall) || !TestNotNull(TEXT("the first target spawned"), TargetA)
		|| !TestNotNull(TEXT("the second target spawned"), TargetB))
	{
		return false;
	}

	ACombatProjectile* Pellet = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the pellet spawned"), Pellet))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy;
	if (!M5_031_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2), TEXT("wall lane")))
	{
		return false;
	}
	Policy.AdvanceMotion(0.25f); // 1500cm requested; the wall front face stops the pellet at ~755cm
	TestTrue(TEXT("the wall terminated the pellet"), Policy.IsFinished());
	TestEqual(TEXT("no pierce slot consumed by the wall"), Policy.GetNumPiercedHits(), 0);
	TestEqual(TEXT("no ledger event submitted"), Scene.Ledger.GetNumRecordedEvents(), 0);
	TestEqual(TEXT("the target behind the wall untouched"), TargetA->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestEqual(TEXT("the farther target untouched"), TargetB->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestTrue(TEXT("the body stopped at the wall's surface"), M5_031_StoppedAtSurface(*Pellet, *Wall));

	// The pass-through-walls context is an illegal combination: Begin refuses.
	ACombatProjectile* Pellet2 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("the refusal probe pellet spawned"), Pellet2))
	{
		return false;
	}
	FProjectileHitContext PassWallContext = M5_031_MakeHitContext(Scene, 2);
	PassWallContext.bUnregisteredActorsBlockShot = false;
	FPenetrationProjectilePolicy Policy2;
	FPenetrationPolicyConfig Config2;
	TestFalse(TEXT("a pass-through-walls context is refused"), Policy2.Configure(Config2) && Policy2.Begin(*Pellet2, FVector(1.0, 0.0, 0.0), PassWallContext));
	TestTrue(TEXT("the refused bind leaves the policy finished"), Policy2.IsFinished());
	TestTrue(TEXT("the refused bind moves nothing"), Pellet2->GetActorLocation().Equals(M5_031_SceneBase + Lane));
	return true;
}

// ---------------------------------------------------------------------------
// AlreadyHitEntityIgnoredMultiBodyAndRecontact
// ---------------------------------------------------------------------------

// The per-projectile hit set: a two-body actor is charged exactly once (the
// compound bounds resume never re-charges the second body), and an entity
// teleported back into the flight line after its hit is passed through
// WITHOUT a second submission and WITHOUT consuming a pierce slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031AlreadyHitEntityIgnoredMultiBodyAndRecontact,
	"UEMMO.Tasks.M5_031.AlreadyHitEntityIgnoredMultiBodyAndRecontact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031AlreadyHitEntityIgnoredMultiBodyAndRecontact::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// --- Sub-scene A (lane Y=12000): one entity, two bodies, one charge. ---
	const FVector LaneA = FVector(0.0, 12000.0, 0.0);
	AActor* MultiBody = M5_031_SpawnMultiBodyTarget(*this, *World, Scene, M5_031_SceneBase + LaneA + FVector(1000.0, 0.0, 0.0));
	AActor* Behind = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneA + FVector(2500.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("the multi-body target spawned"), MultiBody) || !TestNotNull(TEXT("the target behind spawned"), Behind))
	{
		return false;
	}
	ACombatProjectile* Pellet = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneA, Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("lane A pellet spawned"), Pellet))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy;
	if (!M5_031_Begin(*this, Policy, *Pellet, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 1), TEXT("multi-body lane")))
	{
		return false;
	}
	Policy.AdvanceMotion(0.5f); // 3000cm: pierces the multi-body target, terminates at the one behind
	TestTrue(TEXT("the pellet finished at the second entity"), Policy.IsFinished());
	TestEqual(TEXT("exactly two pierce slots consumed"), Policy.GetNumPiercedHits(), 2);
	TestEqual(TEXT("exactly two ledger events"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestEqual(TEXT("the two-body entity charged exactly once"), MultiBody->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the entity behind damaged once"), Behind->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestTrue(TEXT("the second event keyed to the entity behind"),
		Scene.Ledger.HasRecordedEvent(M5_031_MakeKey(Scene, Scene.IdOf(*Behind), 0)));
	TestEqual(TEXT("the finished pellet released its hit set"), Policy.GetNumDistinctTargetsHit(), 0);

	// --- Sub-scene B (lane Y=15000): a hit entity walked back into the line is passed, not re-charged. ---
	const FVector LaneB = FVector(0.0, 15000.0, 0.0);
	AActor* Walker = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* Farther = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(3000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("the walker target spawned"), Walker) || !TestNotNull(TEXT("the farther target spawned"), Farther))
	{
		return false;
	}
	ACombatProjectile* Pellet2 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneB, Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("lane B pellet spawned"), Pellet2))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy2;
	if (!M5_031_Begin(*this, Policy2, *Pellet2, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 3), TEXT("re-contact lane")))
	{
		return false;
	}
	Policy2.AdvanceMotion(0.3f); // 1800cm: hits the walker (submission 1), flies on to 1800
	TestEqual(TEXT("the walker damaged once"), Walker->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the live hit set tracks the walker"), Policy2.GetNumDistinctTargetsHit(), 1);
	Walker->SetActorLocation(M5_031_SceneBase + LaneB + FVector(2000.0, 0.0, 0.0)); // back into the flight line
	Policy2.AdvanceMotion(0.15f); // 900cm: re-contacts the walker - passed without a submission or a slot
	TestEqual(TEXT("the re-contact did not charge the walker again"), Walker->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the re-contact consumed no pierce slot"), Policy2.GetNumPiercedHits(), 1);
	Policy2.AdvanceMotion(0.15f); // reaches the farther target: submission 2
	TestEqual(TEXT("the farther target damaged"), Farther->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the pierce accounting counts only real submissions"), Policy2.GetNumPiercedHits(), 2);
	TestEqual(TEXT("exactly two ledger events on the lane"), Scene.Ledger.GetNumRecordedEvents(), 4); // 2 (lane A) + 2
	TestFalse(TEXT("the budget is not exhausted, the pellet flies on"), Policy2.IsFinished());
	return true;
}

// ---------------------------------------------------------------------------
// TwoProjectilesIndependent
// ---------------------------------------------------------------------------

// Two pellets of one shot stay independent: each carries its own hit set and
// its own pierce budget, both damage the same target (distinct five-tuple
// keys), and the shot control on that target is accepted exactly once (the
// ledger's pellet-insensitive control dedup).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031TwoProjectilesIndependent,
	"UEMMO.Tasks.M5_031.TwoProjectilesIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031TwoProjectilesIndependent::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	const FVector Lane = FVector(0.0, 18000.0, 0.0);
	AActor* Target = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("the target spawned"), Target))
	{
		return false;
	}

	ACombatProjectile* Pellet1 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 0);
	ACombatProjectile* Pellet2 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("the first pellet spawned"), Pellet1) || !TestNotNull(TEXT("the second pellet spawned"), Pellet2))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy1;
	FPenetrationProjectilePolicy Policy2;
	if (!M5_031_Begin(*this, Policy1, *Pellet1, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 0), TEXT("first pellet"))
		|| !M5_031_Begin(*this, Policy2, *Pellet2, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 0), TEXT("second pellet")))
	{
		return false;
	}
	Policy1.AdvanceMotion(0.25f);
	Policy2.AdvanceMotion(0.25f);
	TestTrue(TEXT("the first pellet finished at the target"), Policy1.IsFinished());
	TestTrue(TEXT("the second pellet finished at the target"), Policy2.IsFinished());
	TestEqual(TEXT("each pellet consumed its own slot"), Policy1.GetNumPiercedHits() + Policy2.GetNumPiercedHits(), 2);
	TestEqual(TEXT("both pellets submitted (distinct keys)"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestTrue(TEXT("the first pellet's key recorded"),
		Scene.Ledger.HasRecordedEvent(M5_031_MakeKey(Scene, Scene.IdOf(*Target), 0)));
	TestTrue(TEXT("the second pellet's key recorded"),
		Scene.Ledger.HasRecordedEvent(M5_031_MakeKey(Scene, Scene.IdOf(*Target), 1)));
	TestEqual(TEXT("the target took both damages"), Target->FindComponentByClass<UHealthComponent>()->GetHealth(), 86.0f);
	TestEqual(TEXT("the shot control accepted exactly once (pellet-insensitive)"), Scene.Ledger.GetNumAcceptedControls(), 1);
	TestEqual(TEXT("the first pellet released its hit set"), Policy1.GetNumDistinctTargetsHit(), 0);
	TestEqual(TEXT("the second pellet released its hit set"), Policy2.GetNumDistinctTargetsHit(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// FriendlyAndCategoryTagFiltering
// ---------------------------------------------------------------------------

// The filtering face: a friendly blocks or passes per the context bit and
// never submits nor consumes; a hostile whose registry category is tag-
// filtered passes without a submission or a slot; without the filter the
// same category is a normal target (the filter is what caused the pass).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031FriendlyAndCategoryTagFiltering,
	"UEMMO.Tasks.M5_031.FriendlyAndCategoryTagFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031FriendlyAndCategoryTagFiltering::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	// --- Sub-scene A (lane Y=21000): a friendly with the block bit terminates the pellet. ---
	const FVector LaneA = FVector(0.0, 21000.0, 0.0);
	AActor* FriendlyA = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneA + FVector(800.0, 0.0, 0.0), TEXT("Player"), TEXT("target"));
	AActor* HostileA = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneA + FVector(1600.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane A friendly spawned"), FriendlyA) || !TestNotNull(TEXT("lane A hostile spawned"), HostileA))
	{
		return false;
	}
	ACombatProjectile* PelletA = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneA, Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("lane A pellet spawned"), PelletA))
	{
		return false;
	}
	FPenetrationProjectilePolicy PolicyA;
	if (!M5_031_Begin(*this, PolicyA, *PelletA, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2), TEXT("friendly-block lane")))
	{
		return false;
	}
	PolicyA.AdvanceMotion(0.4f);
	TestTrue(TEXT("the friendly blocked the pellet"), PolicyA.IsFinished());
	TestEqual(TEXT("the friendly was not damaged"), FriendlyA->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestEqual(TEXT("the hostile behind untouched"), HostileA->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestEqual(TEXT("no submission, no slot"), PolicyA.GetNumPiercedHits(), 0);

	// --- Sub-scene B (lane Y=24000): a friendly with the pass bit is passed without cost. ---
	const FVector LaneB = FVector(0.0, 24000.0, 0.0);
	AActor* FriendlyB = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(800.0, 0.0, 0.0), TEXT("Player"), TEXT("target"));
	AActor* HostileB = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneB + FVector(1600.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane B friendly spawned"), FriendlyB) || !TestNotNull(TEXT("lane B hostile spawned"), HostileB))
	{
		return false;
	}
	ACombatProjectile* PelletB = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneB, Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("lane B pellet spawned"), PelletB))
	{
		return false;
	}
	FProjectileHitContext ContextB = M5_031_MakeHitContext(Scene, 2);
	ContextB.bFriendliesBlockShot = false;
	FPenetrationProjectilePolicy PolicyB;
	if (!M5_031_Begin(*this, PolicyB, *PelletB, FVector(1.0, 0.0, 0.0), ContextB, TEXT("friendly-pass lane")))
	{
		return false;
	}
	PolicyB.AdvanceMotion(0.4f);
	TestEqual(TEXT("the passed friendly was not damaged"), FriendlyB->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestEqual(TEXT("the hostile behind damaged"), HostileB->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the friendly pass consumed no slot"), PolicyB.GetNumPiercedHits(), 1);

	// --- Sub-scene C (lane Y=27000): the category tag filter passes a would-be target. ---
	const FVector LaneC = FVector(0.0, 27000.0, 0.0);
	AActor* Prop = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneC + FVector(800.0, 0.0, 0.0), TEXT("Enemy"), TEXT("prop"));
	AActor* HostileC = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneC + FVector(1600.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane C prop spawned"), Prop) || !TestNotNull(TEXT("lane C hostile spawned"), HostileC))
	{
		return false;
	}
	ACombatProjectile* PelletC = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneC, Scene.ShooterId, 2);
	if (!TestNotNull(TEXT("lane C pellet spawned"), PelletC))
	{
		return false;
	}
	FPenetrationProjectilePolicy PolicyC;
	FPenetrationPolicyConfig ConfigC;
	ConfigC.IgnoreCategories.Add(TEXT("prop"));
	if (!TestTrue(TEXT("lane C: the tag configuration accepted"), PolicyC.Configure(ConfigC))
		|| !TestTrue(TEXT("lane C: the policy began"), PolicyC.Begin(*PelletC, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2))))
	{
		return false;
	}
	PolicyC.AdvanceMotion(0.4f);
	TestEqual(TEXT("the tag-filtered prop was not damaged"), Prop->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestEqual(TEXT("the hostile behind damaged"), HostileC->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the tag filter consumed no slot"), PolicyC.GetNumPiercedHits(), 1);

	// --- Sub-scene D (lane Y=30000): without the filter the same category is a normal target. ---
	const FVector LaneD = FVector(0.0, 30000.0, 0.0);
	AActor* Prop2 = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneD + FVector(800.0, 0.0, 0.0), TEXT("Enemy"), TEXT("prop"));
	AActor* HostileD = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + LaneD + FVector(1600.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("lane D prop spawned"), Prop2) || !TestNotNull(TEXT("lane D hostile spawned"), HostileD))
	{
		return false;
	}
	ACombatProjectile* PelletD = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + LaneD, Scene.ShooterId, 3);
	if (!TestNotNull(TEXT("lane D pellet spawned"), PelletD))
	{
		return false;
	}
	FPenetrationProjectilePolicy PolicyD;
	if (!M5_031_Begin(*this, PolicyD, *PelletD, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2), TEXT("unfiltered lane")))
	{
		return false;
	}
	PolicyD.AdvanceMotion(0.4f);
	TestEqual(TEXT("without the filter the prop is a normal target"), Prop2->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("without the filter the hostile behind is reached too"), HostileD->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("both real targets consumed slots"), PolicyD.GetNumPiercedHits(), 2);
	TestEqual(TEXT("the ledger events of the whole scene"), Scene.Ledger.GetNumRecordedEvents(), 4); // 0 + 1 + 1 + 2
	return true;
}

// ---------------------------------------------------------------------------
// ExplosionPierceCombinationRefusedAndFailClosedBegin
// ---------------------------------------------------------------------------

// The illegal-combination and fail-closed faces: an explosion radius
// combined with a pierce budget refuses the bind (the M5-004 rule enforced
// again at the policy seam), a legal explosion radius with no pierce budget
// binds and functions (the legal control), the numeric/context refusal
// probes leave zero side effects, and Configure refuses while in flight and
// for a negative radius (with a legal control so a stub cannot pass).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031ExplosionPierceCombinationRefusedAndFailClosedBegin,
	"UEMMO.Tasks.M5_031.ExplosionPierceCombinationRefusedAndFailClosedBegin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031ExplosionPierceCombinationRefusedAndFailClosedBegin::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	const FVector Lane = FVector(0.0, 33000.0, 0.0);
	AActor* Target = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("the target spawned"), Target))
	{
		return false;
	}

	// The legal control: explosion radius with NO pierce budget binds and functions.
	ACombatProjectile* Pellet1 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the legal-control pellet spawned"), Pellet1))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy1;
	FPenetrationPolicyConfig ExplosionConfig;
	ExplosionConfig.ExplosionRadiusCm = 300.0f;
	if (!TestTrue(TEXT("the legal explosion configuration accepted"), Policy1.Configure(ExplosionConfig))
		|| !TestTrue(TEXT("the legal control began"), Policy1.Begin(*Pellet1, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 0))))
	{
		return false;
	}
	Policy1.AdvanceMotion(0.25f);
	TestTrue(TEXT("the legal control hit and finished"), Policy1.IsFinished());
	TestEqual(TEXT("the legal control submitted once"), Policy1.GetNumPiercedHits(), 1);
	TestEqual(TEXT("the target damaged by the legal control"), Target->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);

	// The illegal combination: explosion radius + pierce budget refuses the bind.
	ACombatProjectile* Pellet2 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("the illegal-combination pellet spawned"), Pellet2))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy2;
	TestTrue(TEXT("the explosion configuration installs"), Policy2.Configure(ExplosionConfig));
	TestFalse(TEXT("explosion + pierce budget refuses the bind"), Policy2.Begin(*Pellet2, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 2)));
	TestTrue(TEXT("the refused bind leaves the policy finished"), Policy2.IsFinished());
	TestTrue(TEXT("the refused bind moves nothing"), Pellet2->GetActorLocation().Equals(M5_031_SceneBase + Lane));

	// Configure probes: a negative radius is refused, a zero radius (the legal control) is not.
	FPenetrationProjectilePolicy ConfigProbe;
	FPenetrationPolicyConfig NegativeRadius;
	NegativeRadius.ExplosionRadiusCm = -1.0f;
	TestFalse(TEXT("a negative explosion radius is refused"), ConfigProbe.Configure(NegativeRadius));
	FPenetrationPolicyConfig ZeroRadius;
	TestTrue(TEXT("a zero explosion radius (the legal control) is accepted"), ConfigProbe.Configure(ZeroRadius));

	// Configure while in flight is refused.
	ACombatProjectile* Pellet3 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 2);
	if (!TestNotNull(TEXT("the in-flight probe pellet spawned"), Pellet3))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy3;
	if (!M5_031_Begin(*this, Policy3, *Pellet3, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 0), TEXT("in-flight probe")))
	{
		return false;
	}
	FPenetrationPolicyConfig Reconfig;
	TestFalse(TEXT("reconfiguring while in flight is refused"), Policy3.Configure(Reconfig));
	Policy3.AdvanceMotion(0.25f); // the pellet functions with the original configuration
	TestTrue(TEXT("the in-flight probe finished at the target"), Policy3.IsFinished());
	TestEqual(TEXT("the in-flight probe submitted once"), Policy3.GetNumPiercedHits(), 1);

	// Numeric and context refusal probes: each leaves zero side effects.
	const int32 ProbeBaseIndex = 3;
	const int32 NumProbes = 4;
	for (int32 Probe = 0; Probe < NumProbes; ++Probe)
	{
		ACombatProjectile* ProbePellet = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, ProbeBaseIndex + Probe);
		if (!TestNotNull(TEXT("a refusal probe pellet spawned"), ProbePellet))
		{
			return false;
		}
		FProjectileHitContext ProbeContext = M5_031_MakeHitContext(Scene, 2);
		FVector ProbeDirection(1.0, 0.0, 0.0);
		FString What;
		if (Probe == 0) { ProbeContext.PierceCount = -1; What = TEXT("negative pierce"); }
		else if (Probe == 1) { ProbeContext.PierceCount = MaxProjectilePierceCount + 1; What = TEXT("over-bound pierce"); }
		else if (Probe == 2) { ProbeContext.Ledger = nullptr; What = TEXT("missing ledger"); }
		else { ProbeDirection = FVector::ZeroVector; What = TEXT("zero direction"); }
		FPenetrationProjectilePolicy ProbePolicy;
		FPenetrationPolicyConfig ProbeConfig;
		TestFalse(FString::Printf(TEXT("%s refuses the bind"), *What),
			ProbePolicy.Configure(ProbeConfig) && ProbePolicy.Begin(*ProbePellet, ProbeDirection, ProbeContext));
		TestTrue(FString::Printf(TEXT("%s leaves the policy finished"), *What), ProbePolicy.IsFinished());
		TestTrue(FString::Printf(TEXT("%s moves nothing"), *What), ProbePellet->GetActorLocation().Equals(M5_031_SceneBase + Lane));
	}
	TestEqual(TEXT("the refusals submitted nothing"), Scene.Ledger.GetNumRecordedEvents(), 2); // legal control + in-flight probe
	return true;
}

// ---------------------------------------------------------------------------
// EndSetCleanupAndNoResidualCallbacks
// ---------------------------------------------------------------------------

// The cleanup and residual faces: the live hit set tracks distinct damaged
// entities while in flight; an externally destroyed pellet leaves the policy
// a silent no-op (invalidation, not a finished report); a finished pellet
// has released its set, never activated the movement component and never
// touched the engine lifetime.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_031EndSetCleanupAndNoResidualCallbacks,
	"UEMMO.Tasks.M5_031.EndSetCleanupAndNoResidualCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_031EndSetCleanupAndNoResidualCallbacks::RunTest(const FString& Parameters)
{
	UWorld* World = M5_031_AcquireWorld();
	if (!TestNotNull(TEXT("a test world acquired"), World))
	{
		return false;
	}
	FM5_031_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_031_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return false;
	}

	const FVector Lane = FVector(0.0, 36000.0, 0.0);
	AActor* TargetA = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(1000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetB = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(2000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	AActor* TargetC = M5_031_SpawnRegisteredTarget(*this, *World, Scene, M5_031_SceneBase + Lane + FVector(3000.0, 0.0, 0.0), TEXT("Enemy"), TEXT("target"));
	if (!TestNotNull(TEXT("the first target spawned"), TargetA) || !TestNotNull(TEXT("the second target spawned"), TargetB)
		|| !TestNotNull(TEXT("the third target spawned"), TargetC))
	{
		return false;
	}

	// Pellet 1: the live set tracks the damaged entity; destruction is a silent no-op.
	ACombatProjectile* Pellet1 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane, Scene.ShooterId, 0);
	if (!TestNotNull(TEXT("the first pellet spawned"), Pellet1))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy1;
	if (!M5_031_Begin(*this, Policy1, *Pellet1, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 5), TEXT("first pellet")))
	{
		return false;
	}
	Policy1.AdvanceMotion(0.25f); // hits the first target, pierces on
	TestEqual(TEXT("the live hit set tracks the damaged entity"), Policy1.GetNumDistinctTargetsHit(), 1);
	TestEqual(TEXT("the first target damaged"), TargetA->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	World->DestroyActor(Pellet1);
	Policy1.AdvanceMotion(0.25f); // destroyed pellet: a silent no-op, no crash
	TestFalse(TEXT("the invalidation is not a finished report"), Policy1.IsFinished());

	// Pellet 2: the finished pellet released its set and left the engine faces
	// untouched. It spawns past the first target so its line reaches only the
	// second one.
	ACombatProjectile* Pellet2 = M5_031_SpawnProjectile(*this, *World, M5_031_SceneBase + Lane + FVector(1200.0, 0.0, 0.0), Scene.ShooterId, 1);
	if (!TestNotNull(TEXT("the second pellet spawned"), Pellet2))
	{
		return false;
	}
	FPenetrationProjectilePolicy Policy2;
	if (!M5_031_Begin(*this, Policy2, *Pellet2, FVector(1.0, 0.0, 0.0), M5_031_MakeHitContext(Scene, 0), TEXT("second pellet")))
	{
		return false;
	}
	Policy2.AdvanceMotion(0.4f); // hits the second target, N=0 terminates there
	TestTrue(TEXT("the pellet finished at the target"), Policy2.IsFinished());
	TestEqual(TEXT("the finished pellet released its hit set"), Policy2.GetNumDistinctTargetsHit(), 0);
	TestEqual(TEXT("exactly one pierce slot consumed"), Policy2.GetNumPiercedHits(), 1);
	TestEqual(TEXT("exactly two ledger events (one per pellet)"), Scene.Ledger.GetNumRecordedEvents(), 2);
	TestEqual(TEXT("the second target damaged"), TargetB->FindComponentByClass<UHealthComponent>()->GetHealth(), 93.0f);
	TestEqual(TEXT("the third target untouched"), TargetC->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	TestTrue(TEXT("the body stopped at the target's surface"), M5_031_StoppedAtSurface(*Pellet2, *TargetB));
	TestFalse(TEXT("the movement component was never activated (no residual callbacks)"), Pellet2->Movement->IsActive());
	TestEqual(TEXT("the engine lifetime was never touched by the policy"), Pellet2->InitialLifeSpan, 0.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
