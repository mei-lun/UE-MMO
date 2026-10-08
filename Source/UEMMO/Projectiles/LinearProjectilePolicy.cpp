// M5-028: the linear projectile policy (owner 028, the Projectiles/* motion
// face). The one-motion-advancer implementation: per AdvanceMotion the UE
// motion component performs the swept transport of the body from its
// previous to its current position (the whole step is one body-radius sweep,
// so no DeltaSeconds can tunnel a thin wall); sweep hits classify through
// the shared face (self never blocks, friendlies/world block-or-pass per
// filter), a hostile hit submits exactly one unified request with the
// complete five-tuple key and consumes one pierce slot before the pellet
// continues; the pierce budget exhausted terminates the pellet at the hit
// point. No direct health writes, no fake actors, no source JSON.

#include "LinearProjectilePolicy.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

EProjectileHitClass ClassifyProjectileHitActor(const AActor* HitActor, const ACombatProjectile& Projectile,
	const FProjectileHitContext& Context)
{
	if (HitActor == nullptr)
	{
		return EProjectileHitClass::None;
	}
	if (Context.SourceActor.Get() == HitActor)
	{
		return EProjectileHitClass::Self;
	}
	// Fail-closed: without the identity/registry seams a hit can never be
	// proven a target, so it classifies as unregistered environment.
	if (Context.Identity == nullptr || Context.Registry == nullptr)
	{
		return EProjectileHitClass::Unregistered;
	}
	const FEntityId HitEntityId = Context.Identity->ResolveTargetEntityId(*HitActor);
	if (HitEntityId == InvalidCombatTargetId)
	{
		return EProjectileHitClass::Unregistered;
	}
	if (HitEntityId == Projectile.Context.SourceEntityId)
	{
		// Identity-based self guard (independent of the weak reference).
		return EProjectileHitClass::Self;
	}
	const FCombatEntityRecord* HitRecord = Context.Registry->FindEntity(HitEntityId);
	if (HitRecord == nullptr)
	{
		return EProjectileHitClass::Unregistered;
	}
	// The 026 same-faction pattern: both factions known and equal.
	const FCombatEntityRecord* SourceRecord = Context.Registry->FindEntity(Projectile.Context.SourceEntityId);
	const bool bSameFaction = SourceRecord != nullptr
		&& !HitRecord->Metadata.Faction.IsNone()
		&& !SourceRecord->Metadata.Faction.IsNone()
		&& HitRecord->Metadata.Faction == SourceRecord->Metadata.Faction;
	return bSameFaction ? EProjectileHitClass::Friendly : EProjectileHitClass::Hostile;
}

bool FLinearProjectilePolicy::Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
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

	Projectile = &PelletActor;
	Direction = InitialDirection;
	Context = HitContext;
	PiercedHits = 0;
	bFinished = false;

	// The self face: the body never collides with its source actor while
	// moving (the UE ignore list), so the muzzle start never self-blocks.
	if (AActor* Source = Context.SourceActor.Get())
	{
		PelletActor.Body->IgnoreActorWhenMoving(Source, true);
	}
	return true;
}

void FLinearProjectilePolicy::AdvanceMotion(float DeltaSeconds)
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
	// current position through swept segments (segment 1 is the full step;
	// further segments only resume pass-through classifications, never a
	// second integration of the step distance).
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

bool FLinearProjectilePolicy::AdvanceOneSegment(ACombatProjectile& Actor, float RemainingCm, float& OutTraveledCm)
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
	const bool bPassThrough = Class == EProjectileHitClass::Self
		|| (Class == EProjectileHitClass::Friendly && !Context.bFriendliesBlockShot)
		|| (Class == EProjectileHitClass::Unregistered && !Context.bUnregisteredActorsBlockShot);
	if (bPassThrough)
	{
		// Pass through: resume just past the hit actor's bounds and consume
		// the remaining distance from there. Blockers inside the resume
		// corridor within this one advance are not re-detected (documented
		// limitation of the per-advance pass-through resume).
		const float ResumeDistance = ResumePastHitActor(Actor, Hit);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	}
	// Blocking classifications (wall/friendly per filter) stop here WITHOUT
	// a submission - the body stays at the hit point.
	if (Class != EProjectileHitClass::Hostile)
	{
		bFinished = true;
		return false;
	}
	// Hostile: exactly one unified submission per passage, then the pierce
	// budget decides continue-vs-terminate.
	SubmitUnifiedHit(Actor, Hit);
	++PiercedHits;
	if (PiercedHits > Context.PierceCount)
	{
		// The (PierceCount+1)-th hostile hit: terminate at the hit point.
		bFinished = true;
		return false;
	}
	const float ResumeDistance = ResumePastHitActor(Actor, Hit);
	OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
	return true;
}

float FLinearProjectilePolicy::ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit) const
{
	const AActor* HitActor = Hit.GetActor();
	float ResumeDistance = 1.0f;
	if (HitActor != nullptr && Actor.Body != nullptr)
	{
		// Cross the hit actor's full world bounds along the flight direction
		// (the bounds half-extent projected twice covers the full extent for
		// the axis-aligned bodies), then clear the body's own radius.
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

void FLinearProjectilePolicy::SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const
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

bool FLinearProjectilePolicy::IsFinished() const
{
	return bFinished;
}

int32 FLinearProjectilePolicy::GetNumPiercedHits() const
{
	return PiercedHits;
}
