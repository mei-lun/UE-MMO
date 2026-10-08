// M5-032: the explosion motion policy (owner 032, a policy consumer of the
// M5-028 frozen face). One detonation at the first blocking contact (the
// flag rises before any query or callback work), the blast queries the
// radius through the engine overlap face and re-filters every candidate
// through the shared classification, the blast-center -> target-center line
// is occlusion-tested against world static/dynamic, the distance falloff
// scales the attack profile's BaseDamage on a per-target copy (center 1,
// edge EdgeScale, outside the radius nothing), and every surviving hostile
// target is submitted through the unified entry with its own five-tuple key.
// The transport stays the 028 continuous-collision face; no residual
// callbacks; the lifetime stays the engine LifeSpan (027).

#include "ExplosionPolicy.h"

#include "../../Combat/CombatComponent.h"
#include "../../Combat/System/UnifiedHitApplier.h"

#include "Components/SphereComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"

bool FExplosionProjectilePolicy::Configure(const FExplosionPolicyConfig& NewConfig)
{
	// A configuration change while the pellet is in flight would rewrite the
	// rules of a started shot: refused, prior state kept.
	if (!bFinished)
	{
		return false;
	}
	if (!FMath::IsFinite(NewConfig.ExplosionRadiusCm) || NewConfig.ExplosionRadiusCm < 0.0f)
	{
		return false;
	}
	if (!FMath::IsFinite(NewConfig.EdgeScale) || NewConfig.EdgeScale < 0.0f || NewConfig.EdgeScale > 1.0f)
	{
		return false;
	}
	Config = NewConfig;
	return true;
}

bool FExplosionProjectilePolicy::Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
	const FProjectileHitContext& HitContext)
{
	// Everything validates BEFORE any state is taken: a refusal binds
	// nothing and leaves the policy finished.
	if (!IsValid(&PelletActor) || PelletActor.Body == nullptr || PelletActor.Movement == nullptr)
	{
		return false;
	}
	const bool bFiniteDirection = FMath::IsFinite(InitialDirection.X) && FMath::IsFinite(InitialDirection.Y)
		&& FMath::IsFinite(InitialDirection.Z);
	if (!bFiniteDirection || InitialDirection.IsZero())
	{
		return false;
	}
	if (HitContext.Registry == nullptr || HitContext.Ledger == nullptr || HitContext.Identity == nullptr)
	{
		return false;
	}
	// The budget sanity: negative is meaningless, over the M5-004 bound is a
	// source-table error that must stay visible.
	if (HitContext.PierceCount < 0 || HitContext.PierceCount > MaxProjectilePierceCount)
	{
		return false;
	}
	// The M5-004 rule at the blast's own seam: one projectile must never
	// detonate more than once, and the blast never shares a body with
	// piercing - any pierce budget refuses the bind.
	if (HitContext.PierceCount > 0)
	{
		return false;
	}
	// An explosion policy without a radius is a configuration error:
	// fail-closed (Configure keeps a zero legal so the wiring can reuse one
	// policy object with a later good configuration).
	if (Config.ExplosionRadiusCm <= 0.0f)
	{
		return false;
	}

	Projectile = &PelletActor;
	Direction = InitialDirection.GetSafeNormal();
	Context = HitContext;
	Detonations = 0;
	ExplosionSubmissions = 0;
	bDetonated = false;
	bFinished = false;

	// The self face: the body never collides with its source actor while
	// moving (the UE ignore list), so the muzzle start never self-blocks.
	if (AActor* Source = Context.SourceActor.Get())
	{
		PelletActor.Body->IgnoreActorWhenMoving(Source, true);
	}
	return true;
}

void FExplosionProjectilePolicy::AdvanceMotion(float DeltaSeconds)
{
	ACombatProjectile* Actor = Projectile.Get();
	if (bFinished || Actor == nullptr || !IsValid(Actor) || Actor->Body == nullptr || Actor->Movement == nullptr)
	{
		return;
	}
	// A zero/non-finite step (Pause-frozen or degenerate frame) advances
	// nothing.
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f)
	{
		return;
	}
	const float DistanceCm = Actor->LaunchSpeedCmS * DeltaSeconds;
	if (!FMath::IsFinite(DistanceCm) || DistanceCm <= 0.0f)
	{
		return;
	}

	// One logical advance: the body transports from its previous to its
	// current position through swept segments. Each segment meets the
	// nearest blocker first; whatever blocks detonates and ends the advance.
	float RemainingCm = DistanceCm;
	for (int32 Segment = 0; Segment < MaxProjectileSegmentsPerAdvance && RemainingCm > KINDA_SMALL_NUMBER; ++Segment)
	{
		float TraveledCm = 0.0f;
		if (!AdvanceOneSegment(*Actor, RemainingCm, TraveledCm))
		{
			return; // detonated at the blocking contact point
		}
		RemainingCm -= TraveledCm;
	}
}

bool FExplosionProjectilePolicy::AdvanceOneSegment(ACombatProjectile& Actor, float RemainingCm, float& OutTraveledCm)
{
	// The UE motion component advances once: the whole remaining step is one
	// body-radius swept move (previous->current), hits decided by the
	// distance along the path.
	FHitResult Hit;
	Actor.Movement->MoveUpdatedComponent(Direction * RemainingCm, FRotator::ZeroRotator, /*bSweep*/ true, &Hit);
	if (!Hit.bBlockingHit)
	{
		OutTraveledCm = RemainingCm;
		return true;
	}
	OutTraveledCm = static_cast<float>(Hit.Distance);

	AActor* HitActor = Hit.GetActor();
	const EProjectileHitClass Class = ClassifyProjectileHitActor(HitActor, Actor, Context);
	if (Class == EProjectileHitClass::Self)
	{
		// The shot's own body (defensive: the source is already on the UE
		// ignore list): pass through, never a detonation.
		const float ResumeDistance = ResumePastHitActor(Actor, Hit);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	}

	// The context filter bits decide whether the class blocks at all;
	// whatever blocks detonates the blast at the contact point, whatever
	// passes just resumes the flight. A hostile body always blocks (and so
	// always detonates) - a blast round has no pierce budget.
	const bool bBlocks = (Class == EProjectileHitClass::Hostile)
		|| (Class == EProjectileHitClass::Unregistered && Context.bUnregisteredActorsBlockShot)
		|| (Class == EProjectileHitClass::Friendly && Context.bFriendliesBlockShot);
	if (!bBlocks)
	{
		const float ResumeDistance = ResumePastHitActor(Actor, Hit);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	}
	Detonate(Actor, Hit.ImpactPoint);
	return false;
}

float FExplosionProjectilePolicy::ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit) const
{
	const AActor* HitActor = Hit.GetActor();
	float ResumeDistance = 1.0f;
	if (HitActor != nullptr && Actor.Body != nullptr)
	{
		// Cross the hit actor's full world bounds along the flight direction,
		// then clear the body's own radius (the 028/031 resume face).
		const FBox Bounds = HitActor->GetComponentsBoundingBox();
		if (Bounds.IsValid)
		{
			const float SphereRadius = Actor.Body->GetUnscaledSphereRadius();
			ResumeDistance = 2.0f * FVector::DotProduct(Bounds.GetExtent(), Direction.GetAbs()) + SphereRadius + 1.0f;
		}
	}
	const FVector ResumeLocation = Hit.ImpactPoint + Direction * ResumeDistance;
	Actor.SetActorLocation(ResumeLocation, /*bSweep*/ false);
	return ResumeDistance;
}

void FExplosionProjectilePolicy::Detonate(ACombatProjectile& Actor, const FVector& BlastCenter)
{
	// The one-detonation flag goes up BEFORE any query or callback work: a
	// re-entered advance or a second blocking body can never repeat the
	// blast.
	bDetonated = true;
	bFinished = true;
	++Detonations;

	UWorld* World = Actor.GetWorld();
	if (World == nullptr || Context.Registry == nullptr || Context.Ledger == nullptr || Context.Identity == nullptr)
	{
		return; // Fail-closed: an incomplete context never invents a submission.
	}

	// The radius query: pawn object types (registered bodies), the pellet
	// itself and its source ignored.
	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams ObjectParams(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(M5_032_Explosion), /*bTraceComplex*/ false);
	QueryParams.AddIgnoredActor(&Actor);
	if (AActor* Source = Context.SourceActor.Get())
	{
		QueryParams.AddIgnoredActor(Source);
	}
	const FCollisionShape Shape = FCollisionShape::MakeSphere(Config.ExplosionRadiusCm);
	if (!World->OverlapMultiByObjectType(Overlaps, BlastCenter, FQuat::Identity, ObjectParams, Shape, QueryParams))
	{
		return; // Nothing inside the radius: the blast happened, it just hit no one.
	}

	// One candidate per actor (a multi-body actor overlaps several times and
	// is submitted exactly once).
	TSet<AActor*> Seen;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Candidate = Overlap.GetActor();
		if (Candidate == nullptr || Seen.Contains(Candidate))
		{
			continue;
		}
		Seen.Add(Candidate);

		// The shared classification re-filters: only hostile registered
		// bodies pay - the source (Self), same-faction bodies (Friendly) and
		// unregistered environment are never damaged.
		if (ClassifyProjectileHitActor(Candidate, Actor, Context) != EProjectileHitClass::Hostile)
		{
			continue;
		}
		const FEntityId TargetId = Context.Identity->ResolveTargetEntityId(*Candidate);
		if (TargetId == InvalidCombatTargetId)
		{
			continue; // Fail-closed: no id, no submission.
		}

		const FVector TargetCenter = Candidate->GetComponentsBoundingBox().GetCenter();
		const float TargetDistance = FVector::Dist(BlastCenter, TargetCenter);
		if (TargetDistance > Config.ExplosionRadiusCm)
		{
			continue; // Defensive: outside the radius is never submitted.
		}

		// The occlusion line: blast center -> target center, world static +
		// world dynamic blockers (pawns never block a blast line). The start
		// sits 1cm in so a blast point on a wall surface does not re-hit the
		// wall it detonated against.
		const FVector ToTarget = TargetCenter - BlastCenter;
		const float Distance = ToTarget.Size();
		if (Distance > KINDA_SMALL_NUMBER)
		{
			const FVector DirectionToTarget = ToTarget / Distance;
			FCollisionObjectQueryParams OcclusionParams;
			OcclusionParams.AddObjectTypesToQuery(ECC_WorldStatic);
			OcclusionParams.AddObjectTypesToQuery(ECC_WorldDynamic);
			FHitResult OcclusionHit;
			if (World->LineTraceSingleByObjectType(OcclusionHit, BlastCenter + DirectionToTarget * 1.0f, TargetCenter,
				OcclusionParams, QueryParams)
				&& OcclusionHit.bBlockingHit)
			{
				continue; // Occluded: the wall eats the blast for this target.
			}
		}

		// The distance falloff: center 1, edge EdgeScale, linear in between -
		// scaled onto a per-target COPY of the context profile.
		const float DistanceT = FMath::Clamp(TargetDistance / Config.ExplosionRadiusCm, 0.0f, 1.0f);
		const float Scale = FMath::Lerp(1.0f, Config.EdgeScale, DistanceT);
		FDamageProfile Scaled = Context.AttackProfile;
		Scaled.BaseDamage *= Scale;
		if (Scaled.BaseDamage <= 0.0f)
		{
			continue; // A zero-scale edge has nothing to apply.
		}

		// The unified entry owns dedup/validation/application: one independent
		// five-tuple key per target (the pellet index is shared, the target id
		// differs), never a direct health write.
		FUnifiedHitRequest Request;
		Request.Epoch = Actor.Context.Epoch;
		Request.AttackerEntityId = Actor.Context.SourceEntityId;
		Request.ShotId = Actor.Context.ShotId;
		Request.PelletIndex = Actor.Context.PelletIndex;
		Request.TargetEntityId = TargetId;
		Request.TargetActor = Candidate;
		Request.Attack = Scaled;
		Request.AttackPower = 0.0f;
		Request.HitLocation = TargetCenter;
		if (UCombatComponent* TargetCombat = Candidate->FindComponentByClass<UCombatComponent>())
		{
			Request.TargetReaction = TargetCombat->GetTargetReactionPolicy();
		}
		ApplyUnifiedHit(Request, *Context.Ledger);
		++ExplosionSubmissions;
	}
}

void FExplosionProjectilePolicy::FinishBlocked()
{
	bFinished = true;
}

bool FExplosionProjectilePolicy::IsFinished() const
{
	return bFinished;
}

int32 FExplosionProjectilePolicy::GetNumDetonations() const
{
	return Detonations;
}

int32 FExplosionProjectilePolicy::GetNumExplosionSubmissions() const
{
	return ExplosionSubmissions;
}
