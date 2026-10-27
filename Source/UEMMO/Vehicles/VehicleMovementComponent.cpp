#include "VehicleMovementComponent.h"

#include "CombatVehicle.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"

/**
 * M5-042: implementation of the kinematic ground drive. Rules live in the
 * header comment; the implementation notes:
 * - Every world step is a MoveComponent sweep of the hull root box (the
 *   M5-039 QueryOnly query body): the engine stops the body at the first
 *   blocking contact, so walls and floors are honored by construction and
 *   penetration needs a bug to happen, not a guard.
 * - The vertical pass holds the hull a tiny clearance above the resting
 *   floor. The clearance exists so the NEXT planar sweep (parallel to the
 *   floor) can never register a contact with the surface the hull stands on
 *   - a sweep from an exactly-touching position is touch-classified and
 *   results-dependent, while a 1cm working gap is deterministic.
 * - The planar pass projects the un-travelled motion and the velocity itself
 *   onto the contact plane, which turns a wall hit into either a stop (head
 *   on) or a slide (angled) without a second penetration guard.
 */

DEFINE_LOG_CATEGORY_STATIC(LogUEMMOVehicleMovement, Log, All);

namespace
{
	// The hover gap above the resting floor (see the file comment): big enough
	// to keep the planar sweep clear of the floor, small enough to be invisible
	// next to the 80cm-tall hull.
	constexpr float GroundClearanceCm = 1.0f;

	// The grounded probe searches this far below the hover gap before the hull
	// is declared airborne (driving off support resumes the fall).
	constexpr float GroundProbeDepthCm = 6.0f;

	// Contacts with an upward normal at least this steep count as ground; the
	// vertical pass only grounds on such contacts (a wall graze while falling
	// must not fake a floor).
	constexpr float MinFloorNormalZ = 0.7f;

	// Slide iterations per planar step: wall -> slide -> corner is resolved
	// within three contacts; anything still blocked after that stops (a corner
	// between two walls converges the remainder to ~0 anyway).
	constexpr int32 MaxPlanarSlideIterations = 3;
}

UVehicleMovementComponent::UVehicleMovementComponent()
{
	// The vehicle actor cannot tick (M5-039 built it bCanEverTick=false and
	// this card does not touch CombatVehicle.*); a component tick function is
	// registered independently of the actor's own tick, so the drive carries
	// its own tick and forwards into the public TickMovement entry.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UVehicleMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	TickMovement(DeltaTime);
}

ACombatVehicle* UVehicleMovementComponent::GetVehicle() const
{
	return Cast<ACombatVehicle>(GetOwner());
}

bool UVehicleMovementComponent::Configure(FString* OutError)
{
	const auto Refuse = [OutError](const FString& Why)
	{
		if (OutError != nullptr)
		{
			*OutError = Why;
		}
		return false;
	};

	ACombatVehicle* Hull = GetVehicle();
	if (Hull == nullptr)
	{
		return Refuse(TEXT("the movement component's owner is not an ACombatVehicle"));
	}
	// A terminal hull never re-opens a lifecycle (the M5-039 first-terminal-
	// wins face) - it must not re-open a drive either.
	if (Hull->IsDestroyFinalized() || Hull->IsPendingKillPending())
	{
		return Refuse(TEXT("the hull is destroyed: a destroyed vehicle refuses a movement configuration"));
	}
	if (!Hull->HasVehicleDefinition())
	{
		return Refuse(TEXT("the hull has no vehicle definition loaded: SetVehicleDefinition must run before Configure"));
	}

	const FVehicleDefinition& Definition = Hull->GetVehicleDefinition();
	// The M5-037 validator already guaranteed finite, strictly positive,
	// in-bounds speed/acceleration (SetVehicleDefinition refused anything
	// else), so no second validation runs here.
	MaxSpeedCmS = Definition.MaxSpeedCmS;
	AccelerationCmS2 = Definition.AccelerationCmS2;
	// The definition carries no separate brake field: the first version brakes
	// as hard as it accelerates (symmetric and config-derived).
	BrakingDecelerationCmS2 = Definition.AccelerationCmS2;
	bConfigured = true;

	UE_LOG(LogUEMMOVehicleMovement, Log,
		TEXT("UEMMO M5-042: vehicle drive configured (max %f cm/s, accel %f cm/s^2)."),
		MaxSpeedCmS, AccelerationCmS2);
	return true;
}

bool UVehicleMovementComponent::SetBrakingDecelerationCmS2(float InBrakingCmS2)
{
	// A non-positive or non-finite brake would freeze or poison the coast-down
	// integration; the caller keeps the previous value instead.
	if (!FMath::IsFinite(InBrakingCmS2) || InBrakingCmS2 <= 0.0f)
	{
		UE_LOG(LogUEMMOVehicleMovement, Warning,
			TEXT("UEMMO M5-042: refused braking deceleration %s (must be finite and > 0)."),
			*FString::SanitizeFloat(InBrakingCmS2));
		return false;
	}
	BrakingDecelerationCmS2 = InBrakingCmS2;
	return true;
}

bool UVehicleMovementComponent::IsDriverPresent() const
{
	if (DriverPresenceProvider)
	{
		return DriverPresenceProvider();
	}
	return bDriverPresent;
}

bool UVehicleMovementComponent::IsDriveControlsSuspended() const
{
	if (ControlSuspensionProvider)
	{
		return ControlSuspensionProvider();
	}
	return bControlsSuspended;
}

bool UVehicleMovementComponent::IsDriveInputAccepted() const
{
	// A dead or destroyed hull never drives (contract section 8: the cleanup
	// covers death; the movement authority dies with the hull).
	const ACombatVehicle* Hull = GetVehicle();
	if (Hull == nullptr || Hull->IsPendingKillPending() || !Hull->IsHullAlive())
	{
		return false;
	}
	if (IsDriveControlsSuspended())
	{
		return false;
	}
	return IsDriverPresent();
}

bool UVehicleMovementComponent::SetDriveInput(float X, float Y)
{
	if (!IsDriveInputAccepted())
	{
		// The refusal also drops any previously held throttle: a driver lost
		// mid-drive must coast down, never keep driving on stale input.
		DriveInput = FVector2D::ZeroVector;
		return false;
	}
	// The M1-029 axis sanitation: non-finite counts as 0, out-of-range clamps
	// to the -1..1 stick envelope.
	DriveInput = FVector2D(
		FMath::IsFinite(X) ? FMath::Clamp(X, -1.0f, 1.0f) : 0.0f,
		FMath::IsFinite(Y) ? FMath::Clamp(Y, -1.0f, 1.0f) : 0.0f);
	return true;
}

void UVehicleMovementComponent::TickMovement(float DeltaSeconds)
{
	ACombatVehicle* Hull = GetVehicle();
	UWorld* World = GetWorld();
	if (World == nullptr || Hull == nullptr || Hull->IsPendingKillPending())
	{
		return;
	}
	// A non-finite or non-positive step would poison both integrations; the
	// step is skipped instead of guessed.
	const float DeltaTime = (FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0.0f) ? DeltaSeconds : 0.0f;
	if (DeltaTime <= 0.0f)
	{
		return;
	}

	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Hull->GetRootComponent());
	if (Body == nullptr || !Body->IsQueryCollisionEnabled())
	{
		// Without a query body there is nothing to sweep and nothing to rest
		// on - the drive honestly refuses to simulate.
		return;
	}

	// The authority is re-read here (not only in SetDriveInput): a driver lost
	// between ticks (death, destroy, exit, suspension) drops the held input so
	// the planar pass coasts down instead of driving on.
	if (!IsDriveInputAccepted())
	{
		DriveInput = FVector2D::ZeroVector;
	}

	UpdateVertical(DeltaTime, *Body);
	UpdatePlanar(DeltaTime, *Body);
}

bool UVehicleMovementComponent::ProbeGroundBelow(const UPrimitiveComponent& Body) const
{
	const UWorld* World = Body.GetWorld();
	if (World == nullptr)
	{
		return false;
	}
	// Swept with the hull's own shape and channel, so the probe and the planar
	// sweep agree on what counts as support. The hull itself is ignored (a
	// body never grounds on itself).
	const FVector Start = Body.GetComponentLocation();
	const FVector End = Start - FVector(0.0f, 0.0f, GroundClearanceCm + GroundProbeDepthCm);
	FCollisionQueryParams Params(TEXT("M5_042GroundProbe"), /*bTraceComplex*/ false);
	Params.AddIgnoredActor(Body.GetOwner());
	FHitResult Hit;
	const bool bHit = World->SweepSingleByChannel(Hit, Start, End, FQuat::Identity,
		Body.GetCollisionObjectType(), Body.GetCollisionShape(), Params);
	return bHit && Hit.bBlockingHit;
}

void UVehicleMovementComponent::UpdateVertical(float DeltaSeconds, UPrimitiveComponent& Body)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (bGrounded)
	{
		// The floor hold is collision-driven: the probe keeps the hull
		// grounded only while support is actually below, so Z never fakes a
		// floor (and a driven-off edge resumes the fall).
		if (ProbeGroundBelow(Body))
		{
			VerticalVelocityCmS = 0.0f;
			return;
		}
		bGrounded = false;
	}

	// The world's own gravity value (default -980) - no second gravity source.
	VerticalVelocityCmS += World->GetGravityZ() * DeltaSeconds;

	FHitResult Hit;
	const FQuat KeepRotation = Body.GetComponentTransform().GetRotation();
	Body.MoveComponent(FVector(0.0f, 0.0f, VerticalVelocityCmS * DeltaSeconds), KeepRotation, /*bSweep*/ true, &Hit);
	if (!Hit.bBlockingHit)
	{
		return;
	}

	const float NormalZ = Hit.ImpactNormal.Z;
	if (NormalZ >= MinFloorNormalZ)
	{
		// Floor: the fall ends at the swept contact and the hover lift re-opens
		// the working gap so the next planar sweep cannot touch the surface
		// the hull stands on (see the file comment).
		VerticalVelocityCmS = 0.0f;
		bGrounded = true;
		Body.MoveComponent(FVector(0.0f, 0.0f, GroundClearanceCm), KeepRotation, /*bSweep*/ false);
	}
	else
	{
		// A ceiling (or a corner graze) ends the current vertical motion; the
		// fall resumes next tick with a fresh gravity step. No ground latch.
		VerticalVelocityCmS = 0.0f;
	}
}

void UVehicleMovementComponent::UpdatePlanar(float DeltaSeconds, UPrimitiveComponent& Body)
{
	const float InputSizeSquared = DriveInput.SizeSquared();
	if (bConfigured && InputSizeSquared > UE_SMALL_NUMBER)
	{
		// Diagonal normalization (the M1-029 precedent): a unit input direction
		// times the scalar acceleration, so diagonal input accelerates exactly
		// as hard as straight input, and the whole-vector clamp below keeps the
		// diagonal top speed equal to the straight one.
		const float InvSize = FMath::InvSqrt(InputSizeSquared);
		const FVector2D Direction = DriveInput * InvSize;
		FVector2D Velocity = PlanarVelocity + Direction * (AccelerationCmS2 * DeltaSeconds);
		const float Speed = Velocity.Size();
		if (Speed > MaxSpeedCmS)
		{
			Velocity *= MaxSpeedCmS / Speed;
		}
		PlanarVelocity = Velocity;
	}
	else
	{
		// No (accepted) input: the braking deceleration bleeds the speed to an
		// exact stop - a released hull coasts down, it never freezes mid-slide
		// nor keeps moving forever.
		const float Speed = PlanarVelocity.Size();
		if (Speed <= 0.0f)
		{
			return;
		}
		const float NewSpeed = FMath::Max(0.0f, Speed - BrakingDecelerationCmS2 * DeltaSeconds);
		PlanarVelocity *= NewSpeed / Speed;
		if (PlanarVelocity.IsNearlyZero())
		{
			PlanarVelocity = FVector2D::ZeroVector;
		}
	}

	// The swept slide move: MoveComponent stops the body at the first blocking
	// contact, the un-travelled remainder is re-projected onto the contact
	// plane and re-swept, so a wall either stops the hull or turns it into a
	// slide along the face - by construction never a penetration.
	FVector Remaining(PlanarVelocity.X * DeltaSeconds, PlanarVelocity.Y * DeltaSeconds, 0.0f);
	const FQuat KeepRotation = Body.GetComponentTransform().GetRotation();
	for (int32 Iteration = 0; Iteration < MaxPlanarSlideIterations && Remaining.SizeSquared() > UE_SMALL_NUMBER; ++Iteration)
	{
		FHitResult Hit;
		Body.MoveComponent(Remaining, KeepRotation, /*bSweep*/ true, &Hit);
		if (!Hit.bBlockingHit)
		{
			return;
		}

		const FVector Normal = Hit.ImpactNormal;
		// The into-wall share of the VELOCITY is removed as well: without it
		// the next tick would re-accelerate straight into the face and the
		// hull would grind against the wall instead of resting or sliding.
		FVector Velocity3D(PlanarVelocity.X, PlanarVelocity.Y, 0.0f);
		const float IntoWall = Velocity3D | Normal;
		if (IntoWall < 0.0f)
		{
			Velocity3D -= IntoWall * Normal;
			PlanarVelocity = FVector2D(Velocity3D.X, Velocity3D.Y);
		}

		// The slide for the remainder of this step: the body already stands at
		// the contact (MoveComponent placed it), the leftover distance moves
		// along the contact plane only.
		const float Untravelled = 1.0f - Hit.Time;
		Remaining = (Remaining - ((Remaining | Normal) * Normal)) * Untravelled;
	}
	// Still blocked after the last iteration: the leftover motion is dropped -
	// a two-wall corner stops the hull instead of jittering in the corner.
}
