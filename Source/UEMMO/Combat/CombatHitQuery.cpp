#include "CombatHitQuery.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "CombatGeometry.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "HealthComponent.h"

namespace
{
	// Debug-only reporting of the selected targets: the category defaults to
	// Log verbosity, so the Verbose lines stay silent until a debugging
	// session raises it (e.g. -LogCmds="UEMMOHitQuery Verbose").
	DEFINE_LOG_CATEGORY_STATIC(UEMMOHitQuery, Log, All);
}

TArray<TWeakObjectPtr<AActor>> QueryTargets(UWorld* World, const FCombatHitBox& Box, const AActor* Attacker, int32 AttackerTeam)
{
	TArray<TWeakObjectPtr<AActor>> Targets;

	// Guard clauses first: without a world or with a degenerate box no
	// physics query runs at all (M1-017 deliberately left value validation
	// to this query layer, so it lives here).
	if (World == nullptr || Box.Extent.ContainsNaN() || Box.Extent.GetMin() <= 0.0)
	{
		return Targets;
	}

	// The hit box is world-axis-aligned by contract (unit rotation), so the
	// collision shape uses the half extent as-is around the world center.
	const FCollisionShape Shape = FCollisionShape::MakeBox(Box.Extent);

	// Combatants are pawns or dynamic actors. Static world geometry is never
	// a target and is not even queried; a dynamic actor without a health
	// component is still queried and then filtered below, so re-channeling a
	// prop cannot make it a target.
	FCollisionObjectQueryParams ObjectParams(ECC_Pawn);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

	FCollisionQueryParams Params(FName(TEXT("UEMMOQueryTargets")), /*bTraceComplex*/ false);
	if (Attacker != nullptr)
	{
		// Cheap pre-filter; the identity filter below is the authoritative one.
		Params.AddIgnoredActor(Attacker);
	}

	TArray<FOverlapResult> Overlaps;
	if (!World->OverlapMultiByObjectType(Overlaps, Box.Center, FQuat::Identity, ObjectParams, Shape, Params))
	{
		return Targets;
	}

	// Dedup by actor identity: every overlapping component maps to its owner,
	// so an actor with several overlapping shapes is reported exactly once.
	TSet<AActor*> SeenActors;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (Actor == nullptr)
		{
			continue;
		}
		bool bAlreadySeen = false;
		SeenActors.Add(Actor, &bAlreadySeen);
		if (bAlreadySeen || Actor == Attacker)
		{
			continue;
		}

		// No UHealthComponent (walls, props) or a dead combatant is never a
		// target; the damage itself stays out of this read-only query
		// (M1-019 owns applying damage to the returned targets).
		const UHealthComponent* Health = Actor->FindComponentByClass<UHealthComponent>();
		if (Health == nullptr || !Health->IsAlive())
		{
			continue;
		}

		Targets.Add(Actor);
		UE_LOG(UEMMOHitQuery, Verbose, TEXT("QueryTargets selected %s (AttackerTeam %d is a placeholder until same-team filtering lands)"), *GetNameSafe(Actor), AttackerTeam);
	}

	return Targets;
}
