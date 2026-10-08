#pragma once

#include "CoreMinimal.h"

#include "../CombatProjectile.h"
#include "../LinearProjectilePolicy.h"

/**
 * M5-030: the homing motion policy (interface contract section 1, owner 030,
 * a policy consumer of the M5-028 frozen face). Implements IProjectileMotionPolicy
 * exactly - one Begin bind, one AdvanceMotion(DeltaSeconds) per tick, one
 * IsFinished report - and plugs in through the one policy seam
 * ("027/028定义唯一推进和命中策略接口，029..032仅提供策略").
 *
 * Guidance, not hand-written physics: the position integration stays the 029
 * engine face (ComputeVelocity with the engine gravity chain - the homing
 * motion carries MotionGravityScale 0 - and ComputeMoveDelta, the engine's
 * Velocity Verlet step) and the transport stays the 028 continuous-collision
 * face (one body-radius swept move per segment). The policy's own work is the
 * bounded-turn guidance: per advance the flight direction rotates toward the
 * target by at most TurnRateDegS * DeltaSeconds degrees (constant angular
 * rate = bounded steering acceleration |a| = v * omega), the speed stays
 * exactly the definition's LaunchSpeedCmS forever (the speed limit face), and
 * the resulting velocity is handed to the engine integration faces. The turn
 * curvature is therefore a pure function of the configured turn rate.
 *
 * Target binding (the "Registry/Filter selection" face): the caller binds a
 * target by ENTITY ID only (BindTarget); the policy never stores a raw world
 * address. At Begin the target record must exist in the registry, resolve to
 * a live actor (a logic-only null-actor record or a destroyed actor is
 * refused - the dangling/bare-address prohibition), and carry a DIFFERENT
 * faction than the shot's source (a same-faction body is never tracked). A
 * refused or missing binding refuses Begin - the policy never invents a
 * target and never re-searches: once the target dies, unregisters or leaves
 * the world (checked every advance through the registry, so the weak
 * reference is released immediately), the reference is dropped (the bound id
 * goes invalid) and the configured lost-target action applies: KeepStraight
 * keeps flying the current direction, Destroy terminates the pellet's motion
 * (IsFinished; the actor recycling itself stays the 027 lifetime face).
 *
 * Hit handling reuses the shared 028 face unchanged: ClassifyProjectileHitActor
 * (fail-closed), the complete five-tuple unified submission per hostile
 * passage, the PierceCount budget, self never blocks - one hit result per
 * tick at most. The lifetime stays the 027 engine LifeSpan face: the policy
 * never registers a timer and never touches InitialLifeSpan.
 *
 * No residual motion callbacks: the policy never activates the movement
 * component, registers no delegates and no timers, and holds the pellet only
 * as a weak reference - a destroyed pellet or an unloaded world leaves it a
 * silent no-op. Y depth keeps its full role: directions are used exactly as
 * provided (never projected into a plane).
 */

/** What the pellet does when its tracked target is lost (dies/unregisters/leaves). */
enum class EHomingLostTargetAction : uint8
{
	/**
	 * Release the reference and keep flying the current direction (no further
	 * guidance, no re-search - the flight continues as a straight line).
	 */
	KeepStraight = 0,

	/**
	 * Release the reference and terminate the pellet's motion immediately
	 * (IsFinished goes true; the actor's recycling stays the 027 lifetime face).
	 */
	Destroy = 1
};

/**
 * The homing policy (M5-004's Homing motion, HomingTurnRateDegS): constant
 * speed, bounded-turn guidance onto one registry-selected hostile target,
 * lost-target handling per the configured action, shared hit classification.
 */
class FHomingProjectilePolicy final : public IProjectileMotionPolicy
{
public:
	/**
	 * Configures the guidance face (call before Begin; refused while a pellet
	 * is in flight). The turn rate must be a finite positive value to pass
	 * Begin - a zero turn rate can never guide (the definition validator
	 * already requires HomingTurnRateDegS > 0 for homing projectiles).
	 */
	void Configure(float InTurnRateDegS, EHomingLostTargetAction InLostAction);

	/**
	 * Registers the tracked target by entity id (call before Begin; refused
	 * while a pellet is in flight or for the invalid id). Value-only: the
	 * policy stores the id and re-resolves the weak reference through the
	 * registry every advance - never a raw world address. The legality
	 * checks (registered, resolvable actor, not friendly) happen at Begin
	 * where the registry seam is available.
	 */
	bool BindTarget(FEntityId Candidate);

	bool Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
		const FProjectileHitContext& HitContext) override;

	void AdvanceMotion(float DeltaSeconds) override;

	bool IsFinished() const override;

	/** Hostile passages consumed so far (the pierce accounting, diagnostics). */
	int32 GetNumPiercedHits() const;

	/** True when the bound target was lost in flight (the reference is released). */
	bool HasLostTarget() const;

	/** The bound target id; Invalid once lost, never bound, or after a refusal. */
	FEntityId GetBoundTargetEntityId() const;

private:
	/**
	 * One guidance step: re-resolves the target through the registry (a dead,
	 * unregistered or off-world target releases the reference immediately and
	 * applies the lost-target action - Destroy returns false to stop the
	 * advance), then rotates the flight direction toward the target by at
	 * most TurnRateDegS * DeltaSeconds degrees.
	 */
	bool UpdateGuidance(ACombatProjectile& Actor, float DeltaSeconds);

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

	/** The current flight direction (rotated per advance; unit length). */
	FVector Direction = FVector::ZeroVector;

	/** The hit context snapshot taken at Begin. */
	FProjectileHitContext Context;

	/** The configured turn rate (degrees per second); 0 until configured. */
	float TurnRateDegS = 0.0f;

	/** The configured lost-target action. */
	EHomingLostTargetAction LostAction = EHomingLostTargetAction::KeepStraight;

	/** The bound target entity id (value only; Invalid after loss). */
	FEntityId TargetEntityId = InvalidCombatEntityId;

	/** True once the target was lost in flight (the reference is released). */
	bool bLostTarget = false;

	/** True when the pellet is done (also the not-begun state). */
	bool bFinished = true;

	/** Hostile passages consumed (the pierce budget accounting). */
	int32 PiercedHits = 0;
};
