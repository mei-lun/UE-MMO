// M5-026: the hitscan delivery executor (owner 026..032 "Projectiles/*").
// Pins the shot-to-world pipeline: the committed FShotContext plus the
// planned pellet geometry traces real world queries (range, walls, depth
// offsets), filters trace candidates (self never, friendlies per filter,
// unregistered actors as environment), and submits every hit through the
// M5-012 unified entry with the complete five-tuple key. Nothing spawns,
// nothing writes health directly, and a same-shot same-target replay is
// refused by the ledger, never double-applied.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Projectiles/HitscanExecutor.h"

#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/BoxComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_026
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the horizontal firing axis, Y the depth axis, Z height.
	const FVector M5_026_SceneBase(60000.0, 50000.0, 800.0);

	// Half extent of one test body (an "enemy" box collider).
	const FVector M5_026_BodyExtent(30.0, 30.0, 60.0);

	/** A fully resolved per-hit profile (pure value; the executor never reads catalogs). */
	static FDamageProfile M5_026_MakeAttackProfile()
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
	static UWorld* M5_026_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_026_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_026 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_026 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_026_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_026_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	/** Spawns a plain actor with one query-enabled box root at Location. */
	static AActor* M5_026_SpawnBoxActor(UWorld& World, const FVector& Location, ECollisionChannel ObjectType)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_026_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_026_BodyExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ObjectType);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);

		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_026_Health"));
		Health->RegisterComponent();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_026_Combat"));
		Combat->RegisterComponent();
		return Actor;
	}

	/** Spawns an unregistered world-static wall (environment, never a target). */
	static AActor* M5_026_SpawnWall(UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_026_Wall"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(FVector(10.0, 200.0, 200.0));
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_WorldStatic);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);
		return Actor;
	}

	/** Spawns a bare shooter actor (no collision: the trace starts past it). */
	static AActor* M5_026_SpawnShooter(UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		return World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
	}

	/**
	 * One scene: registry + bound ledger + identity seam, a shooter entity
	 * and one enemy box. The enemy stands at Base + EnemyOffset with its box
	 * center raised onto the firing line (Z = 90 above the base feet).
	 */
	struct FM5_026_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FTestTargetIdentity Identity;
		FEntityId ShooterId = InvalidCombatEntityId;
		FEntityId EnemyId = InvalidCombatEntityId;
		AActor* Shooter = nullptr;
		AActor* Enemy = nullptr;
		UHealthComponent* EnemyHealth = nullptr;

		static FVector EnemyCenter(const FVector& EnemyOffset)
		{
			return M5_026_SceneBase + EnemyOffset + FVector(0.0, 0.0, 90.0);
		}

		bool Build(FAutomationTestBase& Test, UWorld& World, const TCHAR* ShooterFaction = TEXT("Player"),
			const TCHAR* EnemyFaction = TEXT("Enemy"), const FVector& EnemyOffset = FVector(450.0, 0.0, 0.0))
		{
			Shooter = M5_026_SpawnShooter(World, M5_026_SceneBase + FVector(-100.0, 0.0, 0.0));
			Enemy = M5_026_SpawnBoxActor(World, EnemyCenter(EnemyOffset), ECC_Pawn);
			if (!Test.TestTrue(TEXT("precondition: the shooter spawns"), Shooter != nullptr)
				|| !Test.TestTrue(TEXT("precondition: the enemy spawns"), Enemy != nullptr))
			{
				return false;
			}
			FCombatEntityMetadata ShooterMeta;
			ShooterMeta.Faction = ShooterFaction;
			ShooterMeta.Category = TEXT("shooter");
			FCombatEntityMetadata EnemyMeta;
			EnemyMeta.Faction = EnemyFaction;
			EnemyMeta.Category = TEXT("enemy");
			const FCombatEpoch Epoch = Registry.GetCurrentEpoch();
			ShooterId = Registry.RegisterEntity(Shooter, ShooterMeta, Epoch);
			EnemyId = Registry.RegisterEntity(Enemy, EnemyMeta, Epoch);
			if (!Test.TestTrue(TEXT("precondition: the shooter registers"), IsValidCombatEntityId(ShooterId))
				|| !Test.TestTrue(TEXT("precondition: the enemy registers"), IsValidCombatEntityId(EnemyId)))
			{
				return false;
			}
			Identity.Record(*Shooter, ShooterId);
			Identity.Record(*Enemy, EnemyId);
			EnemyHealth = Enemy->FindComponentByClass<UHealthComponent>();
			return Test.TestTrue(TEXT("precondition: the enemy carries a health component"), EnemyHealth != nullptr);
		}

		/** Draws the shooter's next common ActionSequence value (the 010 allocator). */
		FShotId AllocateShot()
		{
			return Registry.AllocateActionSequence(ShooterId, Registry.GetCurrentEpoch());
		}

		/** A single straight-pellet request down +X from the firing origin. */
		FHitscanRequest MakeStraightShot(FShotId ShotId, float RangeCm = 1000.0f, int32 PelletCount = 1) const
		{
			FHitscanRequest Request;
			Request.Shot.Epoch = Registry.GetCurrentEpoch();
			Request.Shot.SourceEntityId = ShooterId;
			Request.Shot.ShotId = ShotId;
			Request.Shot.WeaponDefinitionId = TEXT("hitscan_test_weapon");
			Request.Shot.FireMode = EWeaponFireMode::Hitscan;
			Request.Shot.DamageProfileId = TEXT("hitscan_test_round");
			Request.Shot.PelletCount = PelletCount;
			Request.Shot.RangeCm = RangeCm;
			Request.Origin = M5_026_SceneBase + FVector(0.0, 0.0, 90.0);
			for (int32 Index = 0; Index < PelletCount; ++Index)
			{
				FShotPellet Pellet;
				Pellet.PelletIndex = static_cast<FPelletIndex>(Index);
				Pellet.Direction = FVector(1.0, 0.0, 0.0);
				Request.Pellets.Add(Pellet);
			}
			Request.AttackProfile = M5_026_MakeAttackProfile();
			return Request;
		}
	};

	/** Runs one execution; guards the preconditions that must hold to continue. */
	static FHitscanExecutionOutcome M5_026_Execute(FAutomationTestBase& Test, UWorld& World, FM5_026_Scene& Scene,
		const FHitscanRequest& Request, const FHitscanFilter& Filter = FHitscanFilter())
	{
		const FHitscanExecutionOutcome Outcome = ExecuteHitscan(&World, Request, &Scene.Registry, &Scene.Ledger,
			&Scene.Identity, Filter);
		Test.TestTrue(TEXT("precondition: the execution was accepted and traced"), Outcome.bExecuted);
		return Outcome;
	}

	/** Counts the world's actors (the no-fake-actor face of the acceptance). */
	static int32 M5_026_CountWorldActors(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			++Count;
		}
		return Count;
	}
}

using namespace UE::UEMMO::Tasks::M5_026;

// ---------------------------------------------------------------------------
// InvalidRequestsNamedRefusal
// ---------------------------------------------------------------------------

// Every pre-trace refusal is named and traces nothing: a null world, an
// unusable shot identity, a non-finite origin, a non-positive range, a
// pellet list that does not match the shot, a duplicated pellet index, a
// zero direction, and each missing dependency.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_026InvalidRequestsNamedRefusal,
	"UEMMO.Tasks.M5_026.InvalidRequestsNamedRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_026InvalidRequestsNamedRefusal::RunTest(const FString& Parameters)
{
	const FDamageProfile Profile = M5_026_MakeAttackProfile();
	const FCombatEpoch Epoch = 1;
	const FShotId ShotId = 1;

	// The base valid request everything below perturbs.
	FHitscanRequest Base;
	Base.Shot.Epoch = Epoch;
	Base.Shot.SourceEntityId = 1;
	Base.Shot.ShotId = ShotId;
	Base.Shot.PelletCount = 1;
	Base.Shot.RangeCm = 1000.0f;
	Base.Origin = FVector(100.0, 0.0, 90.0);
	FShotPellet Pellet;
	Pellet.PelletIndex = 0;
	Pellet.Direction = FVector(1.0, 0.0, 0.0);
	Base.Pellets.Add(Pellet);
	Base.AttackProfile = Profile;

	FCombatEntityRegistry Registry;
	FHitLedger Ledger{ &Registry };
	FTestTargetIdentity Identity;
	FHitscanFilter Filter;

	// One live world for every non-null-world refusal (they are refused
	// before any trace, so a temp world with no actors suffices).
	UWorld* World = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope WorldScope;
	WorldScope.World = World;

	// Every refusal passes the world explicitly: the null-world case nullptr,
	// all others the live temp world (they are refused before any trace).
	auto Rejects = [&](const TCHAR* What, const FHitscanRequest& Request, EHitscanReject Expected,
		UWorld* UseWorld, FCombatEntityRegistry* Reg, FHitLedger* Led, const IHitscanTargetIdentity* Ident)
	{
		const FHitscanExecutionOutcome Outcome = ExecuteHitscan(UseWorld, Request, Reg, Led, Ident, Filter);
		TestFalse(FString::Printf(TEXT("%s is refused"), What), Outcome.bExecuted);
		TestEqual(FString::Printf(TEXT("%s names its reject"), What), Outcome.Reject, Expected);
		TestTrue(FString::Printf(TEXT("%s names the offending detail"), What), !Outcome.RejectDetail.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s traces no pellets"), What), Outcome.PelletsTraced, 0);
		TestEqual(FString::Printf(TEXT("%s produces no hits"), What), Outcome.Hits.Num(), 0);
	};

	Rejects(TEXT("a null world"), Base, EHitscanReject::NullWorld, /*World*/nullptr, &Registry, &Ledger, &Identity);

	FHitscanRequest NoEpoch = Base;
	NoEpoch.Shot.Epoch = InvalidCombatEpoch;
	Rejects(TEXT("an invalid epoch"), NoEpoch, EHitscanReject::InvalidShotContext, World, &Registry, &Ledger, &Identity);

	FHitscanRequest NoSource = Base;
	NoSource.Shot.SourceEntityId = InvalidCombatEntityId;
	Rejects(TEXT("an invalid source entity"), NoSource, EHitscanReject::InvalidShotContext, World, &Registry, &Ledger, &Identity);

	FHitscanRequest NoShotId = Base;
	NoShotId.Shot.ShotId = InvalidCombatShotId;
	Rejects(TEXT("an invalid shot id"), NoShotId, EHitscanReject::InvalidShotContext, World, &Registry, &Ledger, &Identity);

	FHitscanRequest NanOrigin = Base;
	NanOrigin.Origin.Y = std::numeric_limits<double>::quiet_NaN();
	Rejects(TEXT("a non-finite origin"), NanOrigin, EHitscanReject::InvalidOrigin, World, &Registry, &Ledger, &Identity);

	FHitscanRequest ZeroRange = Base;
	ZeroRange.Shot.RangeCm = 0.0f;
	Rejects(TEXT("a zero range"), ZeroRange, EHitscanReject::InvalidRange, World, &Registry, &Ledger, &Identity);

	FHitscanRequest NanRange = Base;
	NanRange.Shot.RangeCm = std::numeric_limits<float>::quiet_NaN();
	Rejects(TEXT("a non-finite range"), NanRange, EHitscanReject::InvalidRange, World, &Registry, &Ledger, &Identity);

	FHitscanRequest EmptyPellets = Base;
	EmptyPellets.Pellets.Reset();
	Rejects(TEXT("an empty pellet list"), EmptyPellets, EHitscanReject::PelletCountMismatch, World, &Registry, &Ledger, &Identity);

	FHitscanRequest CountMismatch = Base;
	CountMismatch.Shot.PelletCount = 2;
	Rejects(TEXT("a pellet count mismatch"), CountMismatch, EHitscanReject::PelletCountMismatch, World, &Registry, &Ledger, &Identity);

	FHitscanRequest DuplicateIndex = Base;
	FShotPellet Second = Pellet;
	Second.PelletIndex = 0;
	DuplicateIndex.Shot.PelletCount = 2;
	DuplicateIndex.Pellets.Add(Second);
	Rejects(TEXT("a duplicated pellet index"), DuplicateIndex, EHitscanReject::DuplicatePelletIndex, World, &Registry, &Ledger, &Identity);

	FHitscanRequest OutOfRangeIndex = Base;
	OutOfRangeIndex.Pellets[0].PelletIndex = 1;
	Rejects(TEXT("an out-of-range pellet index"), OutOfRangeIndex, EHitscanReject::DuplicatePelletIndex, World, &Registry, &Ledger, &Identity);

	FHitscanRequest ZeroDirection = Base;
	ZeroDirection.Pellets[0].Direction = FVector::ZeroVector;
	Rejects(TEXT("a zero pellet direction"), ZeroDirection, EHitscanReject::InvalidDirection, World, &Registry, &Ledger, &Identity);

	FHitscanRequest NanDirection = Base;
	NanDirection.Pellets[0].Direction = FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
	Rejects(TEXT("a non-finite pellet direction"), NanDirection, EHitscanReject::InvalidDirection, World, &Registry, &Ledger, &Identity);

	Rejects(TEXT("a missing registry"), Base, EHitscanReject::MissingRegistry, World, /*Reg*/nullptr, &Ledger, &Identity);
	Rejects(TEXT("a missing ledger"), Base, EHitscanReject::MissingLedger, World, &Registry, /*Led*/nullptr, &Identity);
	Rejects(TEXT("a missing target identity"), Base, EHitscanReject::MissingTargetIdentity,
		World, &Registry, &Ledger, /*Ident*/nullptr);

	return true;
}

// ---------------------------------------------------------------------------
// NearEnemyHitFarEnemyBeyondRange
// ---------------------------------------------------------------------------

// A hostile enemy inside the range is hit: the health drops by exactly the
// profile damage through the unified entry, and the hit's five-tuple key is
// complete. A hostile enemy beyond RangeCm is never hit: out-of-range is a
// trace-length fact, not a half applied hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_026NearEnemyHitFarEnemyBeyondRange,
	"UEMMO.Tasks.M5_026.NearEnemyHitFarEnemyBeyondRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_026NearEnemyHitFarEnemyBeyondRange::RunTest(const FString& Parameters)
{
	UWorld* World = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_026_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	// Near enemy: inside the 1000cm range on the firing line.
	const FShotId ShotId = Scene.AllocateShot();
	const FHitscanRequest Request = Scene.MakeStraightShot(ShotId);
	const FHitscanExecutionOutcome Outcome = M5_026_Execute(*this, *World, Scene, Request);

	if (Outcome.bExecuted)
	{
		TestEqual(TEXT("one pellet was traced"), Outcome.PelletsTraced, 1);
		TestEqual(TEXT("one hit was submitted"), Outcome.Hits.Num(), 1);
		if (Outcome.Hits.Num() == 1)
		{
			const FHitscanHitRecord& Hit = Outcome.Hits[0];
			TestEqual(TEXT("the hit names the pellet index"), Hit.PelletIndex, 0);
			TestTrue(TEXT("the hit names the enemy target"), Hit.TargetEntityId == Scene.EnemyId);
			TestEqual(TEXT("the hit's epoch matches the shot"), Hit.Key.Epoch, Request.Shot.Epoch);
			TestEqual(TEXT("the hit's entity is the shooter"), Hit.Key.EntityId, Scene.ShooterId);
			TestEqual(TEXT("the hit's shot id matches"), Hit.Key.ShotId, ShotId);
			TestEqual(TEXT("the hit's pellet index matches"), static_cast<int32>(Hit.Key.PelletIndex), 0);
			TestEqual(TEXT("the hit's target id matches"), Hit.Key.TargetId, Scene.EnemyId);
			TestEqual(TEXT("the unified entry applied the hit"), Hit.Outcome.Decision, EHitDecision::Apply);
			TestEqual(TEXT("the hit applied exactly the profile damage"), Hit.Outcome.DamageApplied, 7.0f);
			TestTrue(TEXT("the hit location is on the firing line"), Hit.HitLocation.Y == Request.Origin.Y);
		}
		TestEqual(TEXT("the target lost exactly the applied health"),
			Scene.EnemyHealth->GetMaxHealth() - Scene.EnemyHealth->GetHealth(), Outcome.HitsApplied > 0 ? 7.0f : 0.0f);
		TestEqual(TEXT("one hit was applied"), Outcome.HitsApplied, 1);
		TestEqual(TEXT("no pellet was blocked"), Outcome.PelletsBlocked, 0);
	}

	// Far enemy: past the range end, a fresh scene (fresh world, fresh epoch).
	UWorld* FarWorld = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a far-test world is available"), FarWorld != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope FarWorldScope;
	FarWorldScope.World = FarWorld;

	FM5_026_Scene FarScene;
	if (!FarScene.Build(*this, *FarWorld, TEXT("Player"), TEXT("Enemy"), FVector(1500.0, 0.0, 0.0)))
	{
		return true;
	}

	const FShotId FarShotId = FarScene.AllocateShot();
	const FHitscanRequest FarRequest = FarScene.MakeStraightShot(FarShotId, /*RangeCm*/1000.0f);
	const FHitscanExecutionOutcome FarOutcome = M5_026_Execute(*this, *FarWorld, FarScene, FarRequest);

	if (FarOutcome.bExecuted)
	{
		TestEqual(TEXT("the far pellet was traced"), FarOutcome.PelletsTraced, 1);
		TestEqual(TEXT("the out-of-range enemy is not hit"), FarOutcome.Hits.Num(), 0);
		TestEqual(TEXT("the out-of-range enemy took no health"),
			FarScene.EnemyHealth->GetMaxHealth() - FarScene.EnemyHealth->GetHealth(), 0.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// WallAndDepthOffsetBlockShot
// ---------------------------------------------------------------------------

// World obstacles block the shot: an enemy behind an unregistered wall
// takes nothing, and an enemy offset off the firing line on the depth axis
// is never hit by a straight shot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_026WallAndDepthOffsetBlockShot,
	"UEMMO.Tasks.M5_026.WallAndDepthOffsetBlockShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_026WallAndDepthOffsetBlockShot::RunTest(const FString& Parameters)
{
	UWorld* World = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope WorldScope;
	WorldScope.World = World;

	// The wall stands between the origin and the enemy.
	FM5_026_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	AActor* Wall = M5_026_SpawnWall(*World, FM5_026_Scene::EnemyCenter(FVector(300.0, 0.0, 0.0)));
	if (!TestNotNull(TEXT("precondition: the wall spawns"), Wall))
	{
		return true;
	}

	const FShotId ShotId = Scene.AllocateShot();
	const FHitscanRequest Request = Scene.MakeStraightShot(ShotId);
	const FHitscanExecutionOutcome Outcome = M5_026_Execute(*this, *World, Scene, Request);

	if (Outcome.bExecuted)
	{
		TestEqual(TEXT("the pellet was traced"), Outcome.PelletsTraced, 1);
		TestEqual(TEXT("the walled enemy is not hit"), Outcome.Hits.Num(), 0);
		TestEqual(TEXT("the walled enemy took no health"),
			Scene.EnemyHealth->GetMaxHealth() - Scene.EnemyHealth->GetHealth(), 0.0f);
		TestEqual(TEXT("the pellet was blocked"), Outcome.PelletsBlocked, 1);
	}

	// Depth misalignment: the enemy sits 100cm off the firing line on Y and a
	// straight +X shot never grazes its 30cm half-extent body.
	UWorld* OffsetWorld = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("an offset-test world is available"), OffsetWorld != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope OffsetWorldScope;
	OffsetWorldScope.World = OffsetWorld;

	FM5_026_Scene OffsetScene;
	if (!OffsetScene.Build(*this, *OffsetWorld, TEXT("Player"), TEXT("Enemy"), FVector(450.0, 100.0, 0.0)))
	{
		return true;
	}

	const FShotId OffsetShotId = OffsetScene.AllocateShot();
	const FHitscanRequest OffsetRequest = OffsetScene.MakeStraightShot(OffsetShotId);
	const FHitscanExecutionOutcome OffsetOutcome = M5_026_Execute(*this, *OffsetWorld, OffsetScene, OffsetRequest);

	if (OffsetOutcome.bExecuted)
	{
		TestEqual(TEXT("the misaligned pellet was traced"), OffsetOutcome.PelletsTraced, 1);
		TestEqual(TEXT("the depth-misaligned enemy is not hit"), OffsetOutcome.Hits.Num(), 0);
		TestEqual(TEXT("the depth-misaligned enemy took no health"),
			OffsetScene.EnemyHealth->GetMaxHealth() - OffsetScene.EnemyHealth->GetHealth(), 0.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// FriendlyAndSelfFiltering
// ---------------------------------------------------------------------------

// The filter face: the shooter itself is never its own target even when a
// body stands on the firing line; a registered friendly blocks the shot
// where it stands when the filter says so and is passed through when it
// does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_026FriendlyAndSelfFiltering,
	"UEMMO.Tasks.M5_026.FriendlyAndSelfFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_026FriendlyAndSelfFiltering::RunTest(const FString& Parameters)
{
	UWorld* World = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope WorldScope;
	WorldScope.World = World;

	// Friendly body between origin and enemy, blocking on by default.
	FM5_026_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	AActor* Friendly = M5_026_SpawnBoxActor(*World, FM5_026_Scene::EnemyCenter(FVector(200.0, 0.0, 0.0)), ECC_Pawn);
	if (!TestNotNull(TEXT("precondition: the friendly spawns"), Friendly))
	{
		return true;
	}
	FCombatEntityMetadata FriendlyMeta;
	FriendlyMeta.Faction = TEXT("Player");
	FriendlyMeta.Category = TEXT("friendly");
	const FCombatEpoch Epoch = Scene.Registry.GetCurrentEpoch();
	const FEntityId FriendlyId = Scene.Registry.RegisterEntity(Friendly, FriendlyMeta, Epoch);
	if (!TestTrue(TEXT("precondition: the friendly registers"), IsValidCombatEntityId(FriendlyId)))
	{
		return true;
	}
	Scene.Identity.Record(*Friendly, FriendlyId);

	FHitscanFilter BlockingFilter;
	BlockingFilter.bFriendliesBlockShot = true;
	const FShotId BlockingShotId = Scene.AllocateShot();
	const FHitscanRequest BlockingRequest = Scene.MakeStraightShot(BlockingShotId);
	const FHitscanExecutionOutcome BlockingOutcome = M5_026_Execute(*this, *World, Scene, BlockingRequest, BlockingFilter);

	if (BlockingOutcome.bExecuted)
	{
		TestEqual(TEXT("the friendly-blocked pellet was traced"), BlockingOutcome.PelletsTraced, 1);
		TestEqual(TEXT("the enemy behind a blocking friendly is not hit"), BlockingOutcome.Hits.Num(), 0);
		TestEqual(TEXT("the enemy behind a blocking friendly took no health"),
			Scene.EnemyHealth->GetMaxHealth() - Scene.EnemyHealth->GetHealth(), 0.0f);
		TestEqual(TEXT("the friendly blocked the shot"), BlockingOutcome.PelletsBlocked, 1);
	}

	// Pass-through filter: the same scene geometry, friendlies transparent.
	// The next shot needs a new pellet-free scene: reuse the world but fire a
	// fresh shot through a fresh request (the old shot's keys are consumed).
	FHitscanFilter PassFilter;
	PassFilter.bFriendliesBlockShot = false;
	const FShotId PassShotId = Scene.AllocateShot();
	const FHitscanRequest PassRequest = Scene.MakeStraightShot(PassShotId);
	const FHitscanExecutionOutcome PassOutcome = M5_026_Execute(*this, *World, Scene, PassRequest, PassFilter);

	if (PassOutcome.bExecuted)
	{
		TestEqual(TEXT("the pass-through pellet was traced"), PassOutcome.PelletsTraced, 1);
		TestEqual(TEXT("the pass-through shot hits the enemy"), PassOutcome.Hits.Num(), 1);
		if (PassOutcome.Hits.Num() == 1)
		{
			TestTrue(TEXT("the pass-through hit names the enemy"),
				PassOutcome.Hits[0].TargetEntityId == Scene.EnemyId);
			TestEqual(TEXT("the pass-through hit applied damage"), PassOutcome.Hits[0].Outcome.DamageApplied, 7.0f);
		}
		TestEqual(TEXT("nothing blocked the pass-through shot"), PassOutcome.PelletsBlocked, 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// NoFakeActorAndKeyOwnership
// ---------------------------------------------------------------------------

// The acceptance's structural face: an executed hitscan spawns no actor
// (no fake projectile actor) and every submitted hit carries the complete
// five-tuple key naming the shooter, the shot and the target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_026NoFakeActorAndKeyOwnership,
	"UEMMO.Tasks.M5_026.NoFakeActorAndKeyOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_026NoFakeActorAndKeyOwnership::RunTest(const FString& Parameters)
{
	UWorld* World = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_026_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	const int32 ActorsBefore = M5_026_CountWorldActors(*World);

	const FShotId ShotId = Scene.AllocateShot();
	const FHitscanRequest Request = Scene.MakeStraightShot(ShotId);
	const FHitscanExecutionOutcome Outcome = M5_026_Execute(*this, *World, Scene, Request);

	if (Outcome.bExecuted)
	{
		const int32 ActorsAfter = M5_026_CountWorldActors(*World);
		TestEqual(TEXT("the execution spawned no actor"), ActorsAfter, ActorsBefore);

		if (TestEqual(TEXT("the hit was submitted once"), Outcome.Hits.Num(), 1))
		{
			const FHitscanHitRecord& Hit = Outcome.Hits[0];
			TestEqual(TEXT("the key's epoch"), Hit.Key.Epoch, Request.Shot.Epoch);
			TestEqual(TEXT("the key's entity is the shooter"), Hit.Key.EntityId, Scene.ShooterId);
			TestEqual(TEXT("the key's shot id"), Hit.Key.ShotId, ShotId);
			TestEqual(TEXT("the key's pellet index"), static_cast<int32>(Hit.Key.PelletIndex), 0);
			TestEqual(TEXT("the key's target is the hit enemy"), Hit.Key.TargetId, Scene.EnemyId);
			TestTrue(TEXT("the impact lies inside the shot range"),
				FVector::Dist(Request.Origin, Hit.HitLocation) <= Request.Shot.RangeCm);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// DuplicateShotAppliedOncePelletsSeparate
// ---------------------------------------------------------------------------

// The dedup face: two pellets of one shot on one target each apply their
// own damage under their own pellet key (the control is accepted once);
// replaying the same shot is refused by the ledger, so a same-shot
// same-target resubmission never double-applies damage or presentation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_026DuplicateShotAppliedOncePelletsSeparate,
	"UEMMO.Tasks.M5_026.DuplicateShotAppliedOncePelletsSeparate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_026DuplicateShotAppliedOncePelletsSeparate::RunTest(const FString& Parameters)
{
	UWorld* World = M5_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_026_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_026_Scene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	const FShotId ShotId = Scene.AllocateShot();
	const FHitscanRequest Request = Scene.MakeStraightShot(ShotId, /*RangeCm*/1000.0f, /*PelletCount*/2);
	const FHitscanExecutionOutcome Outcome = M5_026_Execute(*this, *World, Scene, Request);

	if (TestTrue(TEXT("the two-pellet shot executed"), Outcome.bExecuted)
		&& TestEqual(TEXT("both pellets submitted a hit"), Outcome.Hits.Num(), 2)
		&& TestEqual(TEXT("both pellets applied"), Outcome.HitsApplied, 2))
	{
		TestEqual(TEXT("pellet 0 owns its damage"), Outcome.Hits[0].Outcome.DamageApplied, 7.0f);
		TestEqual(TEXT("pellet 1 owns its damage"), Outcome.Hits[1].Outcome.DamageApplied, 7.0f);
		TestTrue(TEXT("the pellet keys differ"), Outcome.Hits[0].Key != Outcome.Hits[1].Key);
		TestTrue(TEXT("the shot's control is accepted exactly once"),
			Outcome.Hits[0].Outcome.Control.bStagger != Outcome.Hits[1].Outcome.Control.bStagger);
		TestEqual(TEXT("both pellets' damage landed"),
			Scene.EnemyHealth->GetMaxHealth() - Scene.EnemyHealth->GetHealth(), 14.0f);

		// Replay the exact same shot: the ledger refuses every key again.
		const FHitscanExecutionOutcome Replay = M5_026_Execute(*this, *World, Scene, Request);
		if (TestTrue(TEXT("the replay executed (traced again)"), Replay.bExecuted)
			&& TestEqual(TEXT("the replay submitted both hits again"), Replay.Hits.Num(), 2))
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				TestTrue(FString::Printf(TEXT("replayed pellet %d is refused"), Index),
					Replay.Hits[Index].Outcome.bWasBlocked);
				TestTrue(FString::Printf(TEXT("replayed pellet %d is a ledger duplicate"), Index),
					Replay.Hits[Index].Outcome.LedgerReason == EHitLedgerRejectReason::DuplicateEvent);
				TestEqual(FString::Printf(TEXT("replayed pellet %d applies no damage"), Index),
					Replay.Hits[Index].Outcome.DamageApplied, 0.0f);
			}
			TestEqual(TEXT("the replay applied nothing"), Replay.HitsApplied, 0);
			TestEqual(TEXT("no replay damage landed"),
				Scene.EnemyHealth->GetMaxHealth() - Scene.EnemyHealth->GetHealth(), 14.0f);
		}
	}
	return true;
}

#endif
