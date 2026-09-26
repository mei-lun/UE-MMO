#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatHitQuery.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_018
{
	// Scene placement: a remote base so the fallback GWorld path can never
	// collide with arena content, and per-shape offsets in X (horizontal),
	// Y (depth), Z (height).
	const FVector M1_018_SceneBase(50000.0, 50000.0, 800.0);

	// Half extent of the queried hit box: the light_01 attack geometry from
	// the interface contract (half extent (85,50,70), offset (95,0,90) from a
	// feet origin at the scene base).
	const FVector M1_018_HitBoxExtent(85.0, 50.0, 70.0);

	// Half extent of one test body (an "enemy" box collider).
	const FVector M1_018_BodyExtent(30.0, 30.0, 60.0);

	static FCombatHitBox M1_018_MakeSceneHitBox()
	{
		FCombatHitBox Box;
		// Center = feet origin (scene base) + HitOffsetFromFeet (100,0,90)
		// mirrors the light_01 forward-facing layout with X in front.
		Box.Center = M1_018_SceneBase + FVector(100.0, 0.0, 90.0);
		Box.Extent = M1_018_HitBoxExtent;
		return Box;
	}

	static FCollisionObjectQueryParams M1_018_MakeObjectParams()
	{
		// Same query channels as the production query: combatants are pawns or
		// dynamic actors; static world geometry is never a target.
		FCollisionObjectQueryParams Params(ECC_Pawn);
		Params.AddObjectTypesToQuery(ECC_WorldDynamic);
		return Params;
	}

	// World acquisition, tried in this order (the task card asks for both
	// paths and a record of which one ran):
	// 1. A private temp world via UWorld::CreateWorld (engine standard,
	//    physics scene created, never ticking, not in the engine world list).
	// 2. The live game world (GWorld) when temp creation is unavailable.
	// The chosen source is logged so the task report can quote it.
	static UWorld* M1_018_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_018_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_018 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_018 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Spawns a plain actor with one query-enabled box root at Location.
	// bWithHealth attaches a fresh UHealthComponent; bStartDead kills it
	// through the only damage entry point (ApplyDamage) before the query.
	static AActor* M1_018_SpawnBoxActor(UWorld& World, const FVector& Location, bool bWithHealth, bool bStartDead = false, ECollisionChannel ObjectType = ECC_Pawn)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_018_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_018_BodyExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ObjectType);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);

		if (bWithHealth)
		{
			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M1_018_Health"));
			Health->RegisterComponent();
			if (bStartDead)
			{
				Health->ApplyDamage(Health->GetMaxHealth());
			}
		}
		return Actor;
	}

	// Attaches one extra query-enabled box to the same actor so two shapes of
	// one actor overlap the query box (per-actor dedup must report it once).
	static void M1_018_AttachExtraBox(AActor& Actor, const FVector& RelativeLocation)
	{
		UBoxComponent* Extra = NewObject<UBoxComponent>(&Actor, TEXT("M1_018_ExtraBody"));
		Extra->SetupAttachment(Actor.GetRootComponent());
		Extra->SetMobility(EComponentMobility::Movable);
		Extra->SetBoxExtent(FVector(20.0, 20.0, 40.0));
		Extra->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Extra->SetCollisionObjectType(ECC_Pawn);
		Extra->SetCollisionResponseToAllChannels(ECR_Ignore);
		Extra->SetRelativeLocation(RelativeLocation);
		Extra->RegisterComponent();
	}

	// Actor cleanup only matters when the actors live in the shared game
	// world: a private temp world is discarded with the process, and calling
	// DestroyActor on it only logs "World has no context" noise.
	static void M1_018_DestroyActors(UWorld* World, const TArray<AActor*>& Actors)
	{
		if (World == nullptr || World != GWorld)
		{
			return;
		}
		for (AActor* Actor : Actors)
		{
			if (IsValid(Actor))
			{
				Actor->Destroy();
			}
		}
	}

	static bool M1_018_ContainsTarget(const TArray<TWeakObjectPtr<AActor>>& Targets, const AActor* Actor)
	{
		for (const TWeakObjectPtr<AActor>& Target : Targets)
		{
			if (Target.Get() == Actor)
			{
				return true;
			}
		}
		return false;
	}

	// Raw scene overlap used only as a test-side precondition: it proves the
	// spawned shapes physically overlap the query box, so the QueryTargets
	// assertions measure the filters, not missing physics state.
	static int32 M1_018_RawOverlapCount(UWorld& World, const FCombatHitBox& Box, const AActor* Attacker)
	{
		FCollisionQueryParams Params(FName(TEXT("M1_018_RawOverlap")), false);
		if (Attacker != nullptr)
		{
			Params.AddIgnoredActor(Attacker);
		}
		TArray<FOverlapResult> Overlaps;
		World.OverlapMultiByObjectType(Overlaps, Box.Center, FQuat::Identity, M1_018_MakeObjectParams(), FCollisionShape::MakeBox(Box.Extent), Params);
		return Overlaps.Num();
	}
}

using namespace UE::UEMMO::Tasks::M1_018;

// A null World and degenerate boxes (zero / negative / NaN extent component)
// never report targets and never run a physics query.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018NullWorldAndDegenerateBoxReturnNoTargets,
	"UEMMO.Tasks.M1_018.NullWorldAndDegenerateBoxReturnNoTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018NullWorldAndDegenerateBoxReturnNoTargets::RunTest(const FString& Parameters)
{
	const FCombatHitBox ValidBox = M1_018_MakeSceneHitBox();

	const TArray<TWeakObjectPtr<AActor>> NullWorldTargets = QueryTargets(nullptr, ValidBox, nullptr, 0);
	TestEqual(TEXT("a null World returns no targets"), NullWorldTargets.Num(), 0);

	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the degenerate box cases"), World != nullptr))
	{
		return true;
	}

	FCombatHitBox ZeroExtent = ValidBox;
	ZeroExtent.Extent = FVector::ZeroVector;
	TestEqual(TEXT("a zero-extent box returns no targets"), QueryTargets(World, ZeroExtent, nullptr, 0).Num(), 0);

	FCombatHitBox NegativeExtent = ValidBox;
	NegativeExtent.Extent = FVector(85.0, -50.0, 70.0);
	TestEqual(TEXT("a negative-extent box returns no targets"), QueryTargets(World, NegativeExtent, nullptr, 0).Num(), 0);

	FCombatHitBox NaNExtent = ValidBox;
	NaNExtent.Extent = FVector(85.0, 50.0, std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("a NaN-extent box returns no targets"), QueryTargets(World, NaNExtent, nullptr, 0).Num(), 0);
	return true;
}

// The acceptance case "an enemy straight ahead at the same depth is returned":
// one alive enemy whose body lies fully inside the box is reported exactly
// once, and nothing else is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018ForwardSameDepthEnemyReturned,
	"UEMMO.Tasks.M1_018.ForwardSameDepthEnemyReturned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018ForwardSameDepthEnemyReturned::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	// The attacker stands far behind the box (no overlap, and no health
	// component either, so it cannot leak into the result from any rule).
	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	AActor* EnemyA = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 90.0), true);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(EnemyA);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the forward enemy spawns"), EnemyA))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	TestEqual(TEXT("precondition: the forward enemy physically overlaps the box"),
		M1_018_RawOverlapCount(*World, Box, Attacker), 1);

	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("exactly one target is returned"), Targets.Num(), 1);
	TestTrue(TEXT("the returned target is the forward same-depth enemy"),
		Targets.Num() == 1 && Targets[0].Get() == EnemyA);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// A body that only partially overlaps the box on the X axis (the horizontal
// front axis) is still returned: the query covers genuine 3D intersection,
// not full containment.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018PartialOverlapEnemyReturned,
	"UEMMO.Tasks.M1_018.PartialOverlapEnemyReturned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018PartialOverlapEnemyReturned::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	// Box X range is [base+15, base+185]; this body spans [base+170, base+230]:
	// a 15 cm partial overlap on X only, inside on Y/Z.
	AActor* EnemyG = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(200.0, 0.0, 90.0), true);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(EnemyG);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the partial-overlap enemy spawns"), EnemyG))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	TestEqual(TEXT("precondition: the partial-overlap enemy physically overlaps the box"),
		M1_018_RawOverlapCount(*World, Box, Attacker), 1);

	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("exactly one target is returned"), Targets.Num(), 1);
	TestTrue(TEXT("the returned target is the partial-overlap enemy"),
		Targets.Num() == 1 && Targets[0].Get() == EnemyG);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// The acceptance case "a 150 cm depth offset and a non-overlapping high
// target are not returned": both bodies are alive enemies, but the box is
// world-axis-aligned across X/Y/Z, so depth and height separation exclude
// them at the physics level already.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018DepthOffsetAndHighAltitudeExcluded,
	"UEMMO.Tasks.M1_018.DepthOffsetAndHighAltitudeExcluded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018DepthOffsetAndHighAltitudeExcluded::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	// Depth offset 150 cm: body Y spans [base+120, base+180] vs box Y
	// [base-50, base+50] - a 70 cm gap, larger than half extent + thickness.
	AActor* EnemyB = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 150.0, 90.0), true);
	// High altitude: body Z spans [base+340, base+460] vs box Z top base+160.
	AActor* EnemyC = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 400.0), true);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(EnemyB);
	Spawned.Add(EnemyC);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the depth-offset enemy spawns"), EnemyB) || !TestNotNull(TEXT("the high-altitude enemy spawns"), EnemyC))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	TestEqual(TEXT("precondition: neither offset body physically overlaps the box"),
		M1_018_RawOverlapCount(*World, Box, Attacker), 0);

	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("a 150 cm depth offset and a high-altitude target are both excluded"), Targets.Num(), 0);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// Dead actors and health-less props are not targets: a killed enemy D and a
// dynamic prop E both physically overlap the box (raw overlap 2), so the
// result must be empty because of the health filters alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018DeadAndHealthlessPropExcluded,
	"UEMMO.Tasks.M1_018.DeadAndHealthlessPropExcluded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018DeadAndHealthlessPropExcluded::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	AActor* EnemyD = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 90.0), true, /*bStartDead*/ true);
	// A dynamic prop with pawn-independent object type and no health component
	// (walls and static meshes are never enemies).
	AActor* PropE = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 90.0), false, false, ECC_WorldDynamic);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(EnemyD);
	Spawned.Add(PropE);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the dead enemy spawns"), EnemyD) || !TestNotNull(TEXT("the health-less prop spawns"), PropE))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	UHealthComponent* EnemyDHealth = EnemyD->FindComponentByClass<UHealthComponent>();
	if (!TestTrue(TEXT("precondition: the dead enemy is dead after lethal ApplyDamage"),
		EnemyDHealth != nullptr && !EnemyDHealth->IsAlive()))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	TestEqual(TEXT("precondition: both the dead enemy and the prop physically overlap the box"),
		M1_018_RawOverlapCount(*World, Box, Attacker), 2);

	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("a dead enemy and a health-less prop are both excluded"), Targets.Num(), 0);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// The acceptance case "an actor with several overlapping components is
// returned exactly once": two box components of one actor both overlap the
// box (raw overlap 2) but the dedup reports the actor one time.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018MultiComponentActorReturnedExactlyOnce,
	"UEMMO.Tasks.M1_018.MultiComponentActorReturnedExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018MultiComponentActorReturnedExactlyOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	AActor* EnemyF = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 90.0), true);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(EnemyF);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the two-component enemy spawns"), EnemyF))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}
	M1_018_AttachExtraBox(*EnemyF, FVector(40.0, 0.0, 0.0));

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	TestEqual(TEXT("precondition: both components of the enemy physically overlap the box"),
		M1_018_RawOverlapCount(*World, Box, Attacker), 2);

	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("the two-component enemy is reported exactly once"), Targets.Num(), 1);
	TestTrue(TEXT("the single reported target is the two-component enemy"),
		Targets.Num() == 1 && Targets[0].Get() == EnemyF);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// The attacker is excluded by identity, not by the health filter: a healthy,
// alive attacker inside its own box is not reported, while the same query
// from another instigator (null attacker) does report that actor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018AttackerSelfExcludedButHittableByOthers,
	"UEMMO.Tasks.M1_018.AttackerSelfExcludedButHittableByOthers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018AttackerSelfExcludedButHittableByOthers::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 90.0), true);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	if (!TestNotNull(TEXT("the healthy attacker spawns"), Attacker))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	// No ignored actor here: the raw count must see the attacker's own shape.
	TestEqual(TEXT("precondition: the attacker physically overlaps its own box"),
		M1_018_RawOverlapCount(*World, Box, nullptr), 1);

	const TArray<TWeakObjectPtr<AActor>> SelfQuery = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("the attacker is not its own target"), SelfQuery.Num(), 0);

	const TArray<TWeakObjectPtr<AActor>> OtherQuery = QueryTargets(World, Box, nullptr, 0);
	TestEqual(TEXT("the same healthy actor is a target for another instigator"), OtherQuery.Num(), 1);
	TestTrue(TEXT("the target of the other instigator is the attacker's actor"),
		OtherQuery.Num() == 1 && OtherQuery[0].Get() == Attacker);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// The real M1-016 enemy class (a Character with a capsule and a health
// component) is found by the query: one ATrainingEnemy straight ahead at the
// same depth is reported exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018TrainingEnemyInFrontIsReturned,
	"UEMMO.Tasks.M1_018.TrainingEnemyInFrontIsReturned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018TrainingEnemyInFrontIsReturned::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	FActorSpawnParameters Params;
	ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(
		ATrainingEnemy::StaticClass(), M1_018_SceneBase + FVector(100.0, 0.0, 90.0), FRotator::ZeroRotator, Params);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(Enemy);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the training enemy spawns"), Enemy))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}
	if (!TestTrue(TEXT("precondition: the training enemy starts alive"),
		Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	TestEqual(TEXT("precondition: the training enemy capsule physically overlaps the box"),
		M1_018_RawOverlapCount(*World, Box, Attacker), 1);

	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, Attacker, 0);
	TestEqual(TEXT("exactly one target is returned"), Targets.Num(), 1);
	TestTrue(TEXT("the returned target is the training enemy"),
		Targets.Num() == 1 && Targets[0].Get() == Enemy);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

// AttackerTeam is a documented placeholder this card: no team source exists
// on actors yet, so different team numbers must not change the result until
// the same-team filtering task lands.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_018TeamParameterIsPlaceholderUntilLaterTask,
	"UEMMO.Tasks.M1_018.TeamParameterIsPlaceholderUntilLaterTask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_018TeamParameterIsPlaceholderUntilLaterTask::RunTest(const FString& Parameters)
{
	UWorld* World = M1_018_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available for the overlap scene"), World != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(-500.0, 0.0, 90.0), false);
	AActor* EnemyA = M1_018_SpawnBoxActor(*World, M1_018_SceneBase + FVector(100.0, 0.0, 90.0), true);
	TArray<AActor*> Spawned;
	Spawned.Add(Attacker);
	Spawned.Add(EnemyA);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the enemy spawns"), EnemyA))
	{
		M1_018_DestroyActors(World, Spawned);
		return true;
	}

	const FCombatHitBox Box = M1_018_MakeSceneHitBox();
	const TArray<TWeakObjectPtr<AActor>> TeamZero = QueryTargets(World, Box, Attacker, 0);
	const TArray<TWeakObjectPtr<AActor>> TeamOther = QueryTargets(World, Box, Attacker, 7);
	TestEqual(TEXT("the placeholder team number does not change the result count"),
		TeamOther.Num(), TeamZero.Num());
	TestTrue(TEXT("both team numbers report the same enemy"),
		TeamZero.Num() == 1 && TeamZero[0].Get() == EnemyA && TeamOther[0].Get() == EnemyA);
	M1_018_DestroyActors(World, Spawned);
	return true;
}

#endif
