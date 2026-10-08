// M5-031: the penetration (bounded pierce) motion policy (owner 031, a policy
// consumer of the M5-028 frozen face). Bounded piercing with the per-projectile
// hit set: the context's PierceCount N is the number of EXTRA hostile targets
// after the first, hits are processed strictly in distance order (each swept
// segment meets the nearest blocker first), an already-damaged entity is never
// re-submitted and never consumes a slot (same-actor multi-body, re-contact),
// world obstacles always block, only real hostile submissions consume slots,
// and the finished pellet releases its set. The transport stays the 028
// continuous-collision face; no residual callbacks; the lifetime stays the
// engine LifeSpan (027).

#include "PenetrationPolicy.h"

#include "../../Combat/CombatComponent.h"
#include "../../Combat/System/CombatEntityRegistry.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

bool FPenetrationProjectilePolicy::Configure(const FPenetrationPolicyConfig& NewConfig)
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
	Config = NewConfig;
	return true;
}

bool FPenetrationProjectilePolicy::Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
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
	// The pierce budget: negative is meaningless, over the M5-004 bound is a
	// source-table error that must stay visible.
	if (HitContext.PierceCount < 0 || HitContext.PierceCount > MaxProjectilePierceCount)
	{
		return false;
	}
	// World obstacles always block for a piercing pellet (M5-系统设计
	// "世界实体阻挡即终止"): a context that would let walls pass is an
	// illegal combination and refuses the bind.
	if (!HitContext.bUnregisteredActorsBlockShot)
	{
		return false;
	}
	// The M5-004 explosion rule enforced again at the policy seam: one
	// projectile must never detonate more than once, so a configured
	// explosion radius never combines with a pierce budget. (The policy
	// never reads catalogs, so the wiring hands over the resolved radius.)
	if (Config.ExplosionRadiusCm > 0.0f && HitContext.PierceCount > 0)
	{
		return false;
	}

	Projectile = &PelletActor;
	Direction = InitialDirection.GetSafeNormal();
	Context = HitContext;
	PiercedHits = 0;
	HitEntities.Reset();
	bFinished = false;

	// The self face: the body never collides with its source actor while
	// moving (the UE ignore list), so the muzzle start never self-blocks.
	if (AActor* Source = Context.SourceActor.Get())
	{
		PelletActor.Body->IgnoreActorWhenMoving(Source, true);
	}
	return true;
}

void FPenetrationProjectilePolicy::AdvanceMotion(float DeltaSeconds)
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
	// nearest blocker first, so the hits of one advance - and across
	// advances - are processed strictly in distance order.
	float RemainingCm = DistanceCm;
	for (int32 Segment = 0; Segment < MaxProjectileSegmentsPerAdvance && RemainingCm > KINDA_SMALL_NUMBER; ++Segment)
	{
		float TraveledCm = 0.0f;
		if (!AdvanceOneSegment(*Actor, RemainingCm, TraveledCm))
		{
			return; // terminated at a blocking hit point
		}
		RemainingCm -= TraveledCm;
	}
}

bool FPenetrationProjectilePolicy::AdvanceOneSegment(ACombatProjectile& Actor, float RemainingCm, float& OutTraveledCm)
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
		// The shot's own body: pass through, never a submission, never a slot.
		const float ResumeDistance = ResumePastHitActor(Actor, Hit);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	}
	if (Class == EProjectileHitClass::Unregistered)
	{
		// World obstacles ALWAYS block a piercing pellet (the context bit
		// was already pinned true at Begin): terminate with no submission
		// and no slot consumed. The body stays at the hit point.
		FinishBlocked();
		return false;
	}
	if (Class == EProjectileHitClass::Friendly)
	{
		if (Context.bFriendliesBlockShot)
		{
			FinishBlocked();
			return false;
		}
		// Passed through per the filter bit: no submission, no slot.
		const float ResumeDistance = ResumePastHitActor(Actor, Hit);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	}

	// Hostile: resolve the entity id (the classification already proved it
	// resolvable; the defensive invalid-id pass keeps the accounting honest
	// if the registry moved between the two seams).
	const FEntityId HitEntityId = HitActor != nullptr ? Context.Identity->ResolveTargetEntityId(*HitActor) : InvalidCombatTargetId;
	const auto PassThrough = [this, &Actor, &Hit, &OutTraveledCm, RemainingCm]()
	{
		const float ResumeDistance = ResumePastHitActor(Actor, Hit);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	};
	if (HitEntityId == InvalidCombatTargetId || HitActor == nullptr)
	{
		return PassThrough();
	}
	// The category tag filter: a would-be target whose registry category is
	// ignored is passed without a submission and without a slot.
	if (Config.IgnoreCategories.Num() > 0)
	{
		const FCombatEntityRecord* HitRecord = Context.Registry->FindEntity(HitEntityId);
		if (HitRecord != nullptr && Config.IgnoreCategories.Contains(HitRecord->Metadata.Category))
		{
			return PassThrough();
		}
	}
	// The per-projectile hit set: an entity this pellet already damaged is
	// ignored - no second submission, no slot (the same-actor multi-body
	// and the re-contact faces; the ledger stays the cross-projectile
	// idempotency authority).
	if (HitEntities.Contains(HitEntityId))
	{
		return PassThrough();
	}
	// A real target: exactly one unified submission per passage, then the
	// pierce budget decides continue-vs-terminate.
	SubmitUnifiedHit(Actor, Hit);
	HitEntities.Add(HitEntityId);
	++PiercedHits;
	if (PiercedHits > Context.PierceCount)
	{
		// The (PierceCount+1)-th hostile hit: terminate at the hit point.
		FinishBlocked();
		return false;
	}
	return PassThrough();
}

float FPenetrationProjectilePolicy::ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit) const
{
	const AActor* HitActor = Hit.GetActor();
	float ResumeDistance = 1.0f;
	if (HitActor != nullptr && Actor.Body != nullptr)
	{
		// Cross the hit actor's full world bounds along the flight direction
		// (the bounds half-extent projected twice covers the full extent for
		// the axis-aligned bodies), then clear the body's own radius. For a
		// multi-body actor the compound bounds span every body, so a second
		// body of the same entity is cleared by the same resume.
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

void FPenetrationProjectilePolicy::SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const
{
	AActor* HitActor = Hit.GetActor();
	if (HitActor == nullptr || Context.Registry == nullptr || Context.Ledger == nullptr || Context.Identity == nullptr)
	{
		return; // Fail-closed: an incomplete context never invents a submission.
	}
	FUnifiedHitRequest Request;
	Request.Epoch = Actor.Context.Epoch;
	Request.AttackerEntityId = Actor.Context.SourceEntityId;
	Request.ShotId = Actor.Context.ShotId;
	Request.PelletIndex = Actor.Context.PelletIndex;
	Request.TargetEntityId = Context.Identity->ResolveTargetEntityId(*HitActor);
	Request.TargetActor = HitActor;
	Request.Attack = Context.AttackProfile;
	Request.AttackPower = 0.0f;
	Request.HitLocation = Hit.ImpactPoint;
	if (UCombatComponent* TargetCombat = HitActor->FindComponentByClass<UCombatComponent>())
	{
		Request.TargetReaction = TargetCombat->GetTargetReactionPolicy();
	}
	// The unified entry owns dedup/validation/application; the policy never
	// touches HealthComponent directly.
	ApplyUnifiedHit(Request, *Context.Ledger);
}

void FPenetrationProjectilePolicy::FinishBlocked()
{
	bFinished = true;
	// The end-set cleanup: the pellet is done, its pierce set is released.
	HitEntities.Reset();
}

bool FPenetrationProjectilePolicy::IsFinished() const
{
	return bFinished;
}

int32 FPenetrationProjectilePolicy::GetNumPiercedHits() const
{
	return PiercedHits;
}

int32 FPenetrationProjectilePolicy::GetNumDistinctTargetsHit() const
{
	return HitEntities.Num();
}
