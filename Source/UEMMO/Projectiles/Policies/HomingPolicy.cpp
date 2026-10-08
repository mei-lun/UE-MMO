// M5-030: the homing motion policy (owner 030, a policy consumer of the
// M5-028 frozen face). The position integration stays the 029 engine face
// (ComputeVelocity with the engine gravity chain - the homing motion carries
// MotionGravityScale 0 - and ComputeMoveDelta, the engine's Velocity Verlet
// step) and the transport stays the 028 continuous-collision face (one
// body-radius swept move per segment, hits classified through the shared
// fail-closed face, a hostile hit submits exactly one unified request with
// the complete five-tuple key and consumes one pierce slot). The policy's
// own work is the bounded-turn guidance: per advance the flight direction
// rotates toward the registry-resolved target by at most
// TurnRateDegS * DeltaSeconds degrees and the speed stays exactly the
// definition's launch speed - the steering acceleration |a| = v * omega is
// bounded by the configuration. The target is held as an entity id only and
// re-resolved through the registry every advance; a dead, unregistered or
// off-world target releases the reference immediately and applies the
// configured lost-target action (keep straight / terminate). The policy
// never re-searches a target, never activates the movement component and
// registers no delegates or timers - a destroyed pellet or an unloaded world
// leaves it a silent no-op.

#include "HomingPolicy.h"

#include "../../Combat/CombatComponent.h"
#include "../../Combat/System/CombatEntityRegistry.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

void FHomingProjectilePolicy::Configure(float InTurnRateDegS, EHomingLostTargetAction InLostAction)
{
	if (!bFinished)
	{
		return; // reconfiguration while a pellet is in flight is refused
	}
	TurnRateDegS = InTurnRateDegS;
	LostAction = InLostAction;
}

bool FHomingProjectilePolicy::BindTarget(FEntityId Candidate)
{
	if (!bFinished || Candidate == InvalidCombatEntityId)
	{
		return false;
	}
	// Value-only registration: the id is re-resolved through the registry at
	// Begin and every advance - the policy never stores a world address.
	TargetEntityId = Candidate;
	bLostTarget = false;
	return true;
}

bool FHomingProjectilePolicy::Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
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
	// The homing faces: a configured turn rate and a bound, registry-legal
	// target are required - the policy never invents a target (fail-closed).
	if (!FMath::IsFinite(TurnRateDegS) || TurnRateDegS <= 0.0f)
	{
		return false;
	}
	if (TargetEntityId == InvalidCombatEntityId)
	{
		return false;
	}
	const FCombatEntityRecord* TargetRecord = HitContext.Registry->FindEntity(TargetEntityId);
	if (TargetRecord == nullptr)
	{
		return false; // never registered or already unregistered
	}
	AActor* TargetActor = TargetRecord->Actor.Get();
	if (TargetActor == nullptr || !IsValid(TargetActor))
	{
		// A logic-only null-actor record or an already-dead actor: no world
		// address to track (the dangling/bare-address prohibition face).
		return false;
	}
	// Never a friendly: a same-faction registered body is refused (the 026
	// same-faction pattern - both factions must be known and equal).
	const FCombatEntityRecord* SourceRecord = HitContext.Registry->FindEntity(PelletActor.Context.SourceEntityId);
	if (SourceRecord != nullptr && !TargetRecord->Metadata.Faction.IsNone()
		&& !SourceRecord->Metadata.Faction.IsNone()
		&& TargetRecord->Metadata.Faction == SourceRecord->Metadata.Faction)
	{
		return false;
	}

	Projectile = &PelletActor;
	Direction = InitialDirection.GetSafeNormal();
	Context = HitContext;
	PiercedHits = 0;
	bLostTarget = false;
	bFinished = false;

	// The engine-parameter face (no hand-written physics): the bound velocity
	// carries the definition's launch speed along the provided direction
	// (homing carries MotionGravityScale 0 - no gravity acceleration).
	PelletActor.Movement->InitialSpeed = 0.0f;
	PelletActor.Movement->MaxSpeed = 0.0f;
	PelletActor.Movement->Velocity = Direction * PelletActor.LaunchSpeedCmS;
	PelletActor.Movement->ProjectileGravityScale = PelletActor.MotionGravityScale;

	// The self face: the body never collides with its source actor while
	// moving (the UE ignore list), so the muzzle start never self-blocks.
	if (AActor* Source = Context.SourceActor.Get())
	{
		PelletActor.Body->IgnoreActorWhenMoving(Source, true);
	}
	return true;
}

bool FHomingProjectilePolicy::UpdateGuidance(ACombatProjectile& Actor, float DeltaSeconds)
{
	if (bLostTarget)
	{
		return true; // already released: the flight continues straight
	}
	// Re-resolve through the registry every advance - the tracked reference
	// is an id, never a world address. A dead, unregistered or off-world
	// target is lost immediately.
	const FCombatEntityRecord* Record = Context.Registry->FindEntity(TargetEntityId);
	AActor* TargetActor = Record != nullptr ? Record->Actor.Get() : nullptr;
	const bool bAlive = Record != nullptr && TargetActor != nullptr && IsValid(TargetActor)
		&& TargetActor->GetWorld() == Actor.GetWorld();
	if (!bAlive)
	{
		// Release the reference now; no re-search ever happens.
		TargetEntityId = InvalidCombatEntityId;
		bLostTarget = true;
		if (LostAction == EHomingLostTargetAction::Destroy)
		{
			bFinished = true;
			return false;
		}
		return true; // KeepStraight: the current direction is kept
	}

	// The bounded turn: rotate the flight direction toward the target by at
	// most TurnRateDegS * DeltaSeconds degrees (constant angular rate - the
	// steering acceleration |a| = v * omega stays bounded by configuration).
	const FVector ToTarget = TargetActor->GetActorLocation() - Actor.GetActorLocation();
	const FVector Desired = ToTarget.GetSafeNormal();
	if (!Desired.IsZero())
	{
		const float CosAngle = FMath::Clamp(FVector::DotProduct(Direction, Desired), -1.0f, 1.0f);
		const float AngleRad = FMath::Acos(CosAngle);
		const float MaxTurnRad = FMath::DegreesToRadians(TurnRateDegS) * DeltaSeconds;
		if (AngleRad <= MaxTurnRad)
		{
			Direction = Desired;
		}
		else if (AngleRad > KINDA_SMALL_NUMBER)
		{
			FVector Axis = FVector::CrossProduct(Direction, Desired);
			if (Axis.IsNearlyZero())
			{
				// Exactly opposite: turn around any perpendicular axis
				// (deterministic - the horizontal axis perpendicular to the
				// flight direction).
				Axis = FVector::CrossProduct(Direction, FVector::UpVector);
				if (Axis.IsNearlyZero())
				{
					Axis = FVector(0.0, 1.0, 0.0); // the flight direction is vertical
				}
			}
			Direction = Direction.RotateAngleAxisRad(MaxTurnRad, Axis.GetSafeNormal()).GetSafeNormal();
		}
	}
	return true;
}

void FHomingProjectilePolicy::AdvanceMotion(float DeltaSeconds)
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

	// The guidance step (may consume the advance entirely through the destroy
	// lost-target action), then the constant launch speed along the guided
	// direction - the speed limit face.
	if (!UpdateGuidance(*Actor, DeltaSeconds))
	{
		return;
	}
	Actor->Movement->Velocity = Direction * Actor->LaunchSpeedCmS;

	// The engine integration step, exactly as the engine's own tick performs
	// it: ComputeVelocity applies the engine gravity chain (0 homing scale =
	// no acceleration), ComputeMoveDelta is the engine's Velocity Verlet step
	// p = v0*t + 0.5*(v1-v0)*t computed from the INITIAL velocity.
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

bool FHomingProjectilePolicy::AdvanceOneSegment(ACombatProjectile& Actor, const FVector& StepDirection,
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

float FHomingProjectilePolicy::ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit,
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

void FHomingProjectilePolicy::SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const
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

bool FHomingProjectilePolicy::IsFinished() const
{
	return bFinished;
}

int32 FHomingProjectilePolicy::GetNumPiercedHits() const
{
	return PiercedHits;
}

bool FHomingProjectilePolicy::HasLostTarget() const
{
	return bLostTarget;
}

FEntityId FHomingProjectilePolicy::GetBoundTargetEntityId() const
{
	return TargetEntityId;
}
