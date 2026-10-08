#pragma once

#include "CoreMinimal.h"

#include "../CombatProjectile.h"
#include "../LinearProjectilePolicy.h"

class AActor;

/**
 * M5-032: the explosion motion policy (interface contract section 1, a policy
 * consumer of the M5-028 frozen face). Implements IProjectileMotionPolicy
 * exactly - one Begin bind, one AdvanceMotion (DeltaSeconds) per tick, one
 * IsFinished report - and plugs in through the one policy seam ("027/028
 * 定义唯一推进和命中策略接口，029..032仅提供策略").
 *
 * The card's faces, pinned here:
 * - One detonation at the first blocking contact: the contact point of the
 *   first blocking sweep hit is the blast center. The detonated flag goes up
 *   BEFORE any query or callback work, so a re-entered AdvanceMotion, a
 *   second blocking body met in the same segment loop or any later advance
 *   can never repeat the blast (GetNumDetonations() stays 0 or 1).
 * - The radius query: the blast queries the world with the engine overlap
 *   face (pawn object types) and re-filters every candidate through the
 *   shared ClassifyProjectileHitActor - only hostile registered bodies are
 *   submitted; the shot's own source, same-faction bodies and unregistered
 *   environment are never damaged (the card's filter face).
 * - Occlusion: the blast center -> target-center line is tested against
 *   world-static and world-dynamic blockers; pawns never block a blast line.
 *   An occluded target takes nothing.
 * - The distance falloff: the blast center scales 1, the blast edge scales
 *   EdgeScale (linear in the distance from the center), and outside the
 *   radius nothing is submitted. The scale multiplies the attack profile's
 *   BaseDamage on a per-target COPY of the context profile - every target
 *   gets its own falloff without touching the shared snapshot.
 * - Life loss only through HitApplication: every damaged target is submitted
 *   through ApplyUnifiedHit with the complete five-tuple key (the pellet
 *   index is shared, the target id differs - one independent key per target),
 *   the ledger stays the idempotency authority and the policy never touches
 *   a HealthComponent.
 * - No pierce/blast sharing: the M5-004 rule enforced at the policy seam - a
 *   context carrying a pierce budget refuses the bind (one projectile must
 *   never detonate more than once, and the blast never shares a body with
 *   piercing).
 *
 * No hand-written physics: the transport stays the 028 continuous-collision
 * face (one body-radius swept move per segment through the UE movement
 * component, hits decided by the distance along the path); no residual
 * callbacks (the movement component is never activated, no delegates, no
 * timers); the lifetime stays the engine LifeSpan (027) - the policy never
 * touches InitialLifeSpan. The pellet is held only as a weak reference: a
 * destroyed pellet leaves the policy a silent no-op.
 */
struct FExplosionPolicyConfig
{
	/**
	 * The blast radius in centimeters. A positive radius is required at Begin
	 * (an explosion policy without a radius is a configuration error -
	 * fail-closed); the wiring hands over the resolved number (the policy
	 * never reads catalogs).
	 */
	float ExplosionRadiusCm = 0.0f;

	/**
	 * The damage scale at the blast edge, in [0, 1]: the blast center scales
	 * 1, the edge scales this value with a linear interpolation in between,
	 * and a target beyond the radius is never submitted. Configure refuses
	 * values outside the range (the falloff never amplifies).
	 */
	float EdgeScale = 0.0f;
};

class FExplosionProjectilePolicy final : public IProjectileMotionPolicy
{
public:
	/**
	 * Installs the definition-derived configuration. Refused (false, prior
	 * state kept) while the policy is in flight (bFinished == false), when
	 * the radius is negative/non-finite or when the edge scale leaves [0, 1].
	 */
	bool Configure(const FExplosionPolicyConfig& NewConfig);

	bool Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
		const FProjectileHitContext& HitContext) override;

	void AdvanceMotion(float DeltaSeconds) override;

	bool IsFinished() const override;

	/**
	 * The one-detonation face: 1 after the pellet detonated, 0 before. A
	 * re-entry or a second blocking contact never raises it again.
	 */
	int32 GetNumDetonations() const;

	/** The hostile submissions the blast made (diagnostics; 0 before the blast). */
	int32 GetNumExplosionSubmissions() const;

private:
	/**
	 * One swept segment of at most RemainingCm along the flight direction.
	 * Returns false when the pellet terminated (detonated at a blocking
	 * contact); otherwise OutTraveledCm carries the consumed distance and the
	 * body sits at the resume position.
	 */
	bool AdvanceOneSegment(ACombatProjectile& Actor, float RemainingCm, float& OutTraveledCm);

	/** Pass-through resume: teleports the body just past the hit actor's bounds. */
	float ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit) const;

	/**
	 * The blast: raises the detonated flag first, then queries the radius,
	 * re-filters, occlusion-tests and submits one unified hit per surviving
	 * hostile target. The center is the contact point of the blocking hit.
	 */
	void Detonate(ACombatProjectile& Actor, const FVector& BlastCenter);

	/** Terminates the pellet (post-blast or post-block). */
	void FinishBlocked();

	/** The bound pellet (invalid after Begin refusal or actor destruction). */
	TWeakObjectPtr<ACombatProjectile> Projectile;

	/** The flight direction (normalized at bind so distance accounting stays exact). */
	FVector Direction = FVector::ZeroVector;

	/** The hit context snapshot taken at Begin. */
	FProjectileHitContext Context;

	/** The definition-derived configuration installed at Configure. */
	FExplosionPolicyConfig Config;

	/** True when the pellet is done (also the not-begun state). */
	bool bFinished = true;

	/** The one-detonation flag (raised before any query or callback work). */
	bool bDetonated = false;

	/** Detonations performed (0 or 1 - the one-blast observability). */
	int32 Detonations = 0;

	/** Hostile submissions the blast made (diagnostics). */
	int32 ExplosionSubmissions = 0;
};

// Compile-time pin: the config stays a value snapshot (the policy never reads
// catalogs, so the wiring hands over the resolved numbers).
static_assert(!std::is_pointer_v<decltype(FExplosionPolicyConfig::ExplosionRadiusCm)>, "FExplosionPolicyConfig must stay a value type");
