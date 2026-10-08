#pragma once

#include "CoreMinimal.h"

#include "../CombatProjectile.h"
#include "../LinearProjectilePolicy.h"

class AActor;

/**
 * M5-029: the ballistic (parabolic) motion policy (interface contract section 1,
 * owner 029, the first policy consumer of the M5-028 frozen face). Implements
 * IProjectileMotionPolicy exactly - one Begin bind, one AdvanceMotion(DeltaSeconds)
 * per tick, one IsFinished report - and plugs in through the one policy seam
 * ("027/028定义唯一推进和命中策略接口，029..032仅提供策略").
 *
 * No hand-written physics integration: the policy only sets the UE movement
 * component's gravity/velocity parameters at Begin (InitialSpeed 0 so the
 * bound Velocity is respected, MaxSpeed 0 = no clamp, Velocity = direction *
 * LaunchSpeedCmS, ProjectileGravityScale = the actor's definition-derived
 * MotionGravityScale) and then drives each advance through the engine's own
 * integration faces - ComputeVelocity (v1 = v0 + gravity*dt with the engine
 * gravity chain GetGravityZ() = world gravity * ProjectileGravityScale, 0
 * scale = zero acceleration) and ComputeMoveDelta (the engine's Velocity
 * Verlet step p = v0*t + 0.5*(v1-v0)*t, exact at step boundaries for constant
 * acceleration). The transport itself stays the 028 continuous-collision face:
 * the UE movement component performs one body-radius swept move per segment,
 * so no step size tunnels a thin wall or floor.
 *
 * Termination faces (the existing policy interfaces, not new ones): the
 * engine LifeSpan owns the lifetime (027, Pause-frozen world clock - the
 * policy never registers a timer); walls/floors classify through the shared
 * M5-028 face (ClassifyProjectileHitActor) and terminate per the context
 * filter bits; a hostile hit submits exactly one unified request with the
 * complete five-tuple key and consumes one pierce slot before the pellet
 * continues or terminates.
 *
 * No residual motion callbacks: the policy never activates the movement
 * component (no component tick is registered anywhere), registers no
 * delegates and no timers, and holds the pellet only as a weak reference -
 * a destroyed pellet or an unloaded world leaves the policy a silent no-op.
 *
 * Y depth keeps its full role: the bound direction is used exactly as
 * provided (never normalized at bind, never projected into a plane); gravity
 * is the engine's world Z acceleration, exactly as UE applies it.
 */
class FBallisticProjectilePolicy final : public IProjectileMotionPolicy
{
public:
	bool Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
		const FProjectileHitContext& HitContext) override;

	void AdvanceMotion(float DeltaSeconds) override;

	bool IsFinished() const override;

	/** Hostile passages consumed so far (the pierce accounting, diagnostics). */
	int32 GetNumPiercedHits() const;

private:
	/**
	 * One swept segment of at most RemainingCm along the current step
	 * direction. Returns false when the pellet terminated (a blocking
	 * classification or the pierce budget ran out); otherwise OutTraveledCm
	 * carries the consumed distance and the body sits at the resume position.
	 */
	bool AdvanceOneSegment(ACombatProjectile& Actor, const FVector& StepDirection, float RemainingCm, float& OutTraveledCm);

	/** Pass-through resume: teleports the body just past the hit actor's bounds. */
	float ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit, const FVector& StepDirection) const;

	/** Submits the hostile hit through the unified entry (complete key). */
	void SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const;

	/** The bound pellet (invalid after Begin refusal or actor destruction). */
	TWeakObjectPtr<ACombatProjectile> Projectile;

	/** The hit context snapshot taken at Begin. */
	FProjectileHitContext Context;

	/** True when the pellet is done (also the not-begun state). */
	bool bFinished = true;

	/** Hostile passages consumed (the pierce budget accounting). */
	int32 PiercedHits = 0;
};
