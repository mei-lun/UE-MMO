// M5-029: the ballistic (parabolic) motion policy (owner 029, a policy
// consumer of the M5-028 frozen face). The policy only sets the UE movement
// component's gravity/velocity parameters at Begin and then drives each
// advance through the engine's own integration faces (ComputeVelocity with
// the engine gravity chain, ComputeMoveDelta with the engine Velocity
// Verlet step) - no hand-written physics integration. The transport stays
// the 028 continuous-collision face: one body-radius swept move per segment
// through the UE movement component, so no step size tunnels a thin wall or
// floor; sweep hits classify through the shared face (self never blocks,
// friendlies/world block-or-pass per filter), a hostile hit submits exactly
// one unified request with the complete five-tuple key and consumes one
// pierce slot before the pellet continues; the pierce budget exhausted
// terminates the pellet at the hit point. The policy never activates the
// movement component and registers no delegates or timers - a destroyed
// pellet or an unloaded world leaves it a silent no-op.

#include "BallisticPolicy.h"

#include "../../Combat/CombatComponent.h"
#include "../../Combat/System/CombatEntityRegistry.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

bool FBallisticProjectilePolicy::Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
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
	Context = HitContext;
	PiercedHits = 0;
	bFinished = false;

	// The engine-parameter face (no hand-written physics): the bound
	// velocity carries the definition's launch speed along the provided
	// direction exactly as it stands (Y depth / Z height keep their role),
	// and the gravity scale mirrors the actor's definition-derived motion
	// field (0 straight, 1 parabolic, 2 heavy). InitialSpeed is zeroed so
	// the component never overrides the bound Velocity, MaxSpeed is zeroed
	// so the engine never clamps the definition's speed.
	PelletActor.Movement->InitialSpeed = 0.0f;
	PelletActor.Movement->MaxSpeed = 0.0f;
	PelletActor.Movement->Velocity = InitialDirection * PelletActor.LaunchSpeedCmS;
	PelletActor.Movement->ProjectileGravityScale = PelletActor.MotionGravityScale;

	// The self face: the body never collides with its source actor while
	// moving (the UE ignore list), so the muzzle start never self-blocks.
	if (AActor* Source = Context.SourceActor.Get())
	{
		PelletActor.Body->IgnoreActorWhenMoving(Source, true);
	}
	return true;
}

void FBallisticProjectilePolicy::AdvanceMotion(float DeltaSeconds)
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

	// The engine integration step, exactly as the engine's own tick performs
	// it: ComputeVelocity applies the engine gravity chain
	// (GetGravityZ() = world gravity * ProjectileGravityScale, 0 scale = no
	// acceleration), ComputeMoveDelta is the engine's Velocity Verlet step
	// p = v0*t + 0.5*(v1-v0)*t (exact at step boundaries for constant
	// acceleration) computed from the INITIAL velocity.
	const FVector InitialVelocity = Actor->Movement->Velocity;
	Actor->Movement->Velocity = Actor->Movement->ComputeVelocity(InitialVelocity, DeltaSeconds);
	const FVector MoveDelta = Actor->Movement->ComputeMoveDelta(InitialVelocity, DeltaSeconds);

	const float StepCm = MoveDelta.Size();
	if (!FMath::IsFinite(StepCm) || StepCm <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const FVector StepDirection = MoveDelta / StepCm;

	// One logical advance: the body transports from its previous to its
	// current position through swept segments (segment 1 is the full step;
	// further segments only resume pass-through classifications, never a
	// second integration of the step distance).
	float RemainingCm = StepCm;
	for (int32 Segment = 0; Segment < MaxProjectileSegmentsPerAdvance && RemainingCm > KINDA_SMALL_NUMBER; ++Segment)
	{
		float TraveledCm = 0.0f;
		if (!AdvanceOneSegment(*Actor, StepDirection, RemainingCm, TraveledCm))
		{
			return; // terminated at a blocking hit point
		}
		RemainingCm -= TraveledCm;
	}
}

bool FBallisticProjectilePolicy::AdvanceOneSegment(ACombatProjectile& Actor, const FVector& StepDirection,
	float RemainingCm, float& OutTraveledCm)
{
	// The UE movement component advances once: the whole remaining step is
	// one body-radius swept move (previous->current), hits decided by the
	// distance along the path.
	FHitResult Hit;
	Actor.Movement->MoveUpdatedComponent(StepDirection * RemainingCm, FRotator::ZeroRotator, /*bSweep*/ true, &Hit);
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
		const float ResumeDistance = ResumePastHitActor(Actor, Hit, StepDirection);
		OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
		return true;
	}
	// Blocking classifications (wall/floor/friendly per filter) stop here
	// WITHOUT a submission - the body stays at the hit point.
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
	const float ResumeDistance = ResumePastHitActor(Actor, Hit, StepDirection);
	OutTraveledCm = FMath::Min(OutTraveledCm + ResumeDistance, RemainingCm);
	return true;
}

float FBallisticProjectilePolicy::ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit,
	const FVector& StepDirection) const
{
	const AActor* HitActor = Hit.GetActor();
	float ResumeDistance = 1.0f;
	if (HitActor != nullptr && Actor.Body != nullptr)
	{
		// Cross the hit actor's full world bounds along the current step
		// direction (the bounds half-extent projected twice covers the full
		// extent for the axis-aligned bodies), then clear the body's own
		// radius.
		const FBox Bounds = HitActor->GetComponentsBoundingBox();
		if (Bounds.IsValid)
		{
			const float SphereRadius = Actor.Body->GetUnscaledSphereRadius();
			ResumeDistance = 2.0f * FVector::DotProduct(Bounds.GetExtent(), StepDirection.GetAbs()) + SphereRadius + 1.0f;
		}
	}
	const FVector ResumeLocation = Hit.ImpactPoint + StepDirection * ResumeDistance;
	Actor.SetActorLocation(ResumeLocation, /*bSweep*/ false);
	return ResumeDistance;
}

void FBallisticProjectilePolicy::SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const
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

bool FBallisticProjectilePolicy::IsFinished() const
{
	return bFinished;
}

int32 FBallisticProjectilePolicy::GetNumPiercedHits() const
{
	return PiercedHits;
}
