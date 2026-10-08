// M5-026: the hitscan delivery executor (owner 026..032 "Projectiles/*").
// Turns one committed shot's geometry into per-pellet world line traces,
// filters the trace candidates (the shooter never, registered friendlies per
// the filter, unregistered bodies as the environment) and submits every hit
// through the M5-012 unified entry with the complete five-tuple key. The
// executor spawns nothing and never touches a HealthComponent: damage and
// control flow exclusively through ApplyUnifiedHit, so the ledger dedup, the
// faction filter and the death/get-up gates stay in the one owned entry.

#include "HitscanExecutor.h"

#include "../Combat/CombatComponent.h"

#include "CollisionQueryParams.h"
#include "Engine/World.h"

namespace UE::UEMMO::Tasks::M5_026
{
	/** True when the value is finite (not NaN, not infinite). */
	template <typename T>
	static bool IsFinite(T Value)
	{
		return FMath::IsFinite(Value);
	}

	/** True when the vector has no NaN/Inf component. */
	static bool IsFiniteVector(const FVector& Vector)
	{
		return IsFinite(Vector.X) && IsFinite(Vector.Y) && IsFinite(Vector.Z);
	}

	/**
	 * The trace object set: combatants are pawns or dynamic actors; static
	 * world geometry (walls) must block the shot without being a target - the
	 * same query split the M1_018 hit query froze.
	 */
	static FCollisionObjectQueryParams MakeObjectParams()
	{
		FCollisionObjectQueryParams Params(ECC_Pawn);
		Params.AddObjectTypesToQuery(ECC_WorldDynamic);
		Params.AddObjectTypesToQuery(ECC_WorldStatic);
		return Params;
	}

	/** Builds the unified request of one hit (the complete five-tuple key). */
	static FUnifiedHitRequest MakeUnifiedRequest(const FHitscanRequest& Request, const FShotPellet& Pellet,
		AActor& HitActor, FEntityId TargetEntityId, const FVector& HitLocation)
	{
		FUnifiedHitRequest Unified;
		Unified.Epoch = Request.Shot.Epoch;
		Unified.AttackerEntityId = Request.Shot.SourceEntityId;
		Unified.ShotId = Request.Shot.ShotId;
		Unified.PelletIndex = Pellet.PelletIndex;
		Unified.TargetEntityId = TargetEntityId;
		Unified.TargetActor = &HitActor;
		Unified.Attack = Request.AttackProfile;
		// The target's own policy (the legacy owner of the reaction values);
		// a target without a combat component takes the default reaction.
		if (UCombatComponent* TargetCombat = HitActor.FindComponentByClass<UCombatComponent>())
		{
			Unified.TargetReaction = TargetCombat->GetTargetReactionPolicy();
		}
		// AttackPower 0 keeps the resolved profile verbatim (the M5-012 rule).
		Unified.AttackPower = 0.0f;
		Unified.HitLocation = HitLocation;
		return Unified;
	}
}

FHitscanExecutionOutcome ExecuteHitscan(UWorld* World, const FHitscanRequest& Request,
	FCombatEntityRegistry* Registry, FHitLedger* Ledger,
	const IHitscanTargetIdentity* TargetIdentity, const FHitscanFilter& Filter)
{
	using namespace UE::UEMMO::Tasks::M5_026;

	FHitscanExecutionOutcome Outcome;

	// Refusal order: everything is validated BEFORE the first trace runs, so
	// a refused request traces nothing and submits nothing.
	if (World == nullptr)
	{
		Outcome.Reject = EHitscanReject::NullWorld;
		Outcome.RejectDetail = TEXT("World");
		return Outcome;
	}
	if (!IsValidCombatEpoch(Request.Shot.Epoch) || !IsValidCombatEntityId(Request.Shot.SourceEntityId)
		|| !IsValidCombatShotId(Request.Shot.ShotId))
	{
		Outcome.Reject = EHitscanReject::InvalidShotContext;
		Outcome.RejectDetail = TEXT("Shot.Epoch/SourceEntityId/ShotId");
		return Outcome;
	}
	if (!IsFiniteVector(Request.Origin))
	{
		Outcome.Reject = EHitscanReject::InvalidOrigin;
		Outcome.RejectDetail = TEXT("Origin");
		return Outcome;
	}
	if (!IsFinite(Request.Shot.RangeCm) || Request.Shot.RangeCm <= 0.0f)
	{
		Outcome.Reject = EHitscanReject::InvalidRange;
		Outcome.RejectDetail = TEXT("Shot.RangeCm");
		return Outcome;
	}
	if (Request.Pellets.Num() == 0 || Request.Pellets.Num() != Request.Shot.PelletCount)
	{
		Outcome.Reject = EHitscanReject::PelletCountMismatch;
		Outcome.RejectDetail = TEXT("Pellets.Num() != Shot.PelletCount");
		return Outcome;
	}
	TSet<FPelletIndex> Seen;
	for (const FShotPellet& Pellet : Request.Pellets)
	{
		if (!IsValidCombatPelletIndex(Pellet.PelletIndex) || Pellet.PelletIndex >= Request.Pellets.Num()
			|| Seen.Contains(Pellet.PelletIndex))
		{
			Outcome.Reject = EHitscanReject::DuplicatePelletIndex;
			Outcome.RejectDetail = FString::Printf(TEXT("PelletIndex %u"), static_cast<uint32>(Pellet.PelletIndex));
			return Outcome;
		}
		Seen.Add(Pellet.PelletIndex);
	}
	for (const FShotPellet& Pellet : Request.Pellets)
	{
		if (!IsFiniteVector(Pellet.Direction) || Pellet.Direction.IsZero())
		{
			Outcome.Reject = EHitscanReject::InvalidDirection;
			Outcome.RejectDetail = FString::Printf(TEXT("PelletIndex %u Direction"), static_cast<uint32>(Pellet.PelletIndex));
			return Outcome;
		}
	}
	if (Registry == nullptr)
	{
		Outcome.Reject = EHitscanReject::MissingRegistry;
		Outcome.RejectDetail = TEXT("Registry");
		return Outcome;
	}
	if (Ledger == nullptr)
	{
		Outcome.Reject = EHitscanReject::MissingLedger;
		Outcome.RejectDetail = TEXT("Ledger");
		return Outcome;
	}
	if (TargetIdentity == nullptr)
	{
		Outcome.Reject = EHitscanReject::MissingTargetIdentity;
		Outcome.RejectDetail = TEXT("TargetIdentity");
		return Outcome;
	}

	// The shooter's own body is never a trace candidate (no self hits and no
	// self blocking); a logic-only source entity simply has none.
	AActor* Shooter = Registry->ResolveEntity(Request.Shot.SourceEntityId).Get();

	FCollisionQueryParams QueryParams(TEXT("M5_026_Hitscan"), /*bTraceComplex*/false);
	if (Shooter != nullptr)
	{
		QueryParams.AddIgnoredActor(Shooter);
	}

	const FCollisionObjectQueryParams ObjectParams = MakeObjectParams();
	const float RangeCm = Request.Shot.RangeCm;

	Outcome.bExecuted = true;

	for (const FShotPellet& Pellet : Request.Pellets)
	{
		++Outcome.PelletsTraced;

		// The unit direction carries the facing; the range caps the segment.
		const FVector End = Request.Origin + Pellet.Direction * RangeCm;

		// A multi trace lets the per-candidate filter decide pass-through vs
		// blocking; the nearest blocker wins, so distance order is fixed here.
		TArray<FHitResult> TraceHits;
		World->LineTraceMultiByObjectType(TraceHits, Request.Origin, End, ObjectParams, QueryParams);
		TraceHits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });

		bool bSettled = false;
		for (const FHitResult& TraceHit : TraceHits)
		{
			AActor* HitActor = TraceHit.GetActor();
			if (HitActor == nullptr || HitActor == Shooter)
			{
				continue;
			}

			const FEntityId TargetEntityId = TargetIdentity->ResolveTargetEntityId(*HitActor);
			const FCombatEntityRecord* TargetRecord = IsValidCombatEntityId(TargetEntityId)
				? Registry->FindEntity(TargetEntityId)
				: nullptr;

			if (TargetRecord == nullptr)
			{
				// Unregistered body: environment, not a target. Blocking is a
				// filter decision (walls always arrive through here too - the
				// trace only ever sees actors with query geometry).
				if (Filter.bUnregisteredActorsBlockShot)
				{
					++Outcome.PelletsBlocked;
					bSettled = true;
					break;
				}
				continue;
			}

			const bool bSameFaction = !TargetRecord->Metadata.Faction.IsNone()
				&& Shooter != nullptr
				&& Registry->FindEntity(Request.Shot.SourceEntityId) != nullptr
				&& TargetRecord->Metadata.Faction == Registry->FindEntity(Request.Shot.SourceEntityId)->Metadata.Faction;
			if (bSameFaction)
			{
				// Friendly fire per filter: block where it stands or pass
				// through - a friendly never takes the hit (the unified entry
				// would refuse it as SameFaction anyway).
				if (Filter.bFriendliesBlockShot)
				{
					++Outcome.PelletsBlocked;
					bSettled = true;
					break;
				}
				continue;
			}

			// Hostile registered target: submit the hit through the unified
			// entry and end this pellet's flight there.
			FUnifiedHitRequest Unified = MakeUnifiedRequest(Request, Pellet, *HitActor, TargetEntityId, TraceHit.ImpactPoint);
			const FUnifiedHitOutcome HitOutcome = ApplyUnifiedHit(Unified, *Ledger);

			FHitscanHitRecord Hit;
			Hit.PelletIndex = Pellet.PelletIndex;
			Hit.Key = Unified.MakeEventKey();
			Hit.TargetEntityId = TargetEntityId;
			Hit.HitLocation = TraceHit.ImpactPoint;
			Hit.Outcome = HitOutcome;
			if (HitOutcome.WasApplied())
			{
				++Outcome.HitsApplied;
			}
			Outcome.Hits.Add(Hit);

			bSettled = true;
			break;
		}

		// A pellet that neither hit a target nor met a blocker simply flew
		// its full range (or passed every candidate through): no record.
		(void)bSettled;
	}

	return Outcome;
}
