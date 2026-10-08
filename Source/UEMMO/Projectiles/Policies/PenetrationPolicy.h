#pragma once

#include "CoreMinimal.h"

#include "../CombatProjectile.h"
#include "../LinearProjectilePolicy.h"

class AActor;

/**
 * M5-031: the penetration (bounded pierce) motion policy (interface contract
 * section 1, a policy consumer of the M5-028 frozen face). Implements
 * IProjectileMotionPolicy exactly - one Begin bind, one AdvanceMotion
 * (DeltaSeconds) per tick, one IsFinished report - and plugs in through the
 * one policy seam ("027/028定义唯一推进和命中策略接口，029..032仅提供策略").
 *
 * The card's faces, pinned here:
 * - Bounded piercing: the context's PierceCount N is the number of EXTRA
 *   hostile targets the pellet may pass through after the first (N=0 wounds
 *   exactly one target, N=2 at most three); hits are processed strictly in
 *   distance order (each swept segment meets the nearest blocker first).
 * - The per-projectile hit set: every entity the pellet already damaged is
 *   remembered by its registry id. A re-contact with the same entity (a
 *   second body of the same actor, the target walking back into the flight
 *   line, a repeated sweep/overlap contact) is passed through WITHOUT a
 *   second unified submission and WITHOUT consuming a pierce slot - the
 *   ledger stays the cross-projectile idempotency authority, the set keeps
 *   the pierce accounting honest. When the pellet finishes the set is
 *   released (the end-set cleanup face, observable through
 *   GetNumDistinctTargetsHit()).
 * - Slot accounting: only a real hostile submission consumes a pierce slot.
 *   Self, unregistered bodies (walls), friendlies and tag-filtered entities
 *   never consume; self and tag-filtered entities never submit, friendlies
 *   submit never and block-or-pass per the context filter bit.
 * - World obstacles always block: the policy refuses a context that would
 *   let unregistered bodies pass (bUnregisteredActorsBlockShot=false is an
 *   illegal combination for a piercing pellet - M5-系统设计 "世界实体阻挡
 *   即终止"), and every unregistered contact terminates the pellet with no
 *   submission and no slot consumed.
 * - The M5-004 explosion rule enforced again at the policy seam: a
 *   configured explosion radius combined with a pierce budget refuses the
 *   bind (one projectile must never detonate more than once; the definition
 *   validator already refuses the pair - this policy refuses it too,
 *   fail-closed, because the wiring may resolve definitions independently).
 *
 * No hand-written physics: the transport stays the 028 continuous-collision
 * face (one body-radius swept move per segment through the UE movement
 * component, hits decided by the distance along the path); no residual
 * callbacks (the movement component is never activated, no delegates, no
 * timers); the lifetime stays the engine LifeSpan (027) - the policy never
 * touches InitialLifeSpan. The pellet is held only as a weak reference: a
 * destroyed pellet leaves the policy a silent no-op.
 */
struct FPenetrationPolicyConfig
{
	/**
	 * Entity categories (the registry metadata's informational tag) the
	 * pellet never damages: a hit actor whose registered category matches any
	 * entry is passed through without a submission and without consuming a
	 * pierce slot (the card's tag filter; the category is the registry's tag
	 * surface - the policy never reads catalogs).
	 */
	TArray<FName> IgnoreCategories;

	/**
	 * The definition's explosion radius in centimeters (0 = no explosion),
	 * passed by the wiring from the resolved definition (the policy never
	 * reads catalogs). A positive radius combined with a pierce budget
	 * refuses the bind - the M5-004 illegal combination, re-enforced here.
	 */
	float ExplosionRadiusCm = 0.0f;
};

class FPenetrationProjectilePolicy final : public IProjectileMotionPolicy
{
public:
	/**
	 * Installs the definition-derived configuration. Refused (false, prior
	 * state kept) while the policy is in flight (bFinished == false) or when
	 * the explosion radius is negative/non-finite. Optional: the default
	 * configuration (no ignored categories, no explosion) is legal.
	 */
	bool Configure(const FPenetrationPolicyConfig& NewConfig);

	bool Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
		const FProjectileHitContext& HitContext) override;

	void AdvanceMotion(float DeltaSeconds) override;

	bool IsFinished() const override;

	/** Hostile passages consumed so far (the pierce accounting, diagnostics). */
	int32 GetNumPiercedHits() const;

	/**
	 * The live size of the per-projectile hit set (distinct entities damaged
	 * and not yet released). 0 after the pellet finished - the end-set
	 * cleanup face.
	 */
	int32 GetNumDistinctTargetsHit() const;

private:
	/**
	 * One swept segment of at most RemainingCm along the flight direction.
	 * Returns false when the pellet terminated (a blocking classification or
	 * the pierce budget ran out); otherwise OutTraveledCm carries the
	 * consumed distance and the body sits at the resume position.
	 */
	bool AdvanceOneSegment(ACombatProjectile& Actor, float RemainingCm, float& OutTraveledCm);

	/** Pass-through resume: teleports the body just past the hit actor's bounds. */
	float ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit) const;

	/** Submits the hostile hit through the unified entry (complete key). */
	void SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const;

	/** Terminates the pellet and releases the hit set (the end-set cleanup). */
	void FinishBlocked();

	/** The bound pellet (invalid after Begin refusal or actor destruction). */
	TWeakObjectPtr<ACombatProjectile> Projectile;

	/** The flight direction (normalized at bind so distance accounting stays exact). */
	FVector Direction = FVector::ZeroVector;

	/** The hit context snapshot taken at Begin. */
	FProjectileHitContext Context;

	/** The definition-derived configuration installed at Configure. */
	FPenetrationPolicyConfig Config;

	/** Entities this pellet already damaged (the per-projectile pierce set). */
	TSet<FEntityId> HitEntities;

	/** True when the pellet is done (also the not-begun state). */
	bool bFinished = true;

	/** Hostile passages consumed (the pierce budget accounting). */
	int32 PiercedHits = 0;
};

// Compile-time pin: the config stays a value snapshot (the policy never reads
// catalogs, so the wiring hands over the resolved numbers).
static_assert(!std::is_pointer_v<decltype(FPenetrationPolicyConfig::ExplosionRadiusCm)>, "FPenetrationPolicyConfig must stay a value type");
