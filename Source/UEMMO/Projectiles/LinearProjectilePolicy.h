#pragma once

#include "CoreMinimal.h"

#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/HitscanExecutor.h"

#include <type_traits>

class AActor;
class FCombatEntityRegistry;

/**
 * M5-028: the projectile motion/hit policy faces (interface contract section
 * 1, owner 028 "Projectiles/*"). 027 built the actor carrier and the spawn
 * transaction; this file freezes the ONE motion/hit policy seam that
 * 029..032 plug policies into ("ProjectileActor在027/028定义唯一推进和命中
 * 策略接口，029..032仅提供策略" - interface contract section 3):
 *
 * - IProjectileMotionPolicy: the one-motion-advancer face. One Begin binds a
 *   pellet actor with its initial direction and hit context; one
 *   AdvanceMotion(DeltaSeconds) call per frame transports the body from its
 *   previous to its current position through exactly one UE motion-component
 *   swept move (previous->current, body-radius sweep, hits decided by the
 *   distance along the path); IsFinished reports a terminated pellet. No
 *   policy may stack a second advancer, a second tick or a second hit result
 *   on one body.
 * - FProjectileHitContext + ClassifyProjectileHitActor: the shared hit
 *   classification value face 029..032 reuse (self/friendly/hostile/
 *   unregistered, fail-closed without the registry seams); the collision-
 *   to-unified-hit conversion submits the complete five-tuple key through
 *   ApplyUnifiedHit - never a direct health write, never a fake actor.
 * - FLinearProjectilePolicy: the reference straight-line implementation -
 *   the swept transport never tunnels a thin wall (the whole previous->
 *   current step is one sweep regardless of DeltaSeconds, so 30/60/120 FPS
 *   steps and 100ms hitches all stop at the first blocker), self never
 *   blocks, friendlies/world block-or-pass per the filter bits and a hostile
 *   hit consumes one pierce slot before the pellet continues.
 *
 * Units (contract section 4): speeds in centimeters per second, times in
 * seconds on the Pause-frozen World clock, lengths in centimeters. The Y
 * depth axis keeps its full role: directions are used exactly as provided
 * (never normalized, never projected into a plane).
 */

/** Classification of one body-sweep hit actor (the shared frozen face). */
enum class EProjectileHitClass : uint8
{
	/** No actor behind the hit (or no hit at all). */
	None = 0,

	/** The shot's own source actor: never blocks, never submits. */
	Self = 1,

	/** A same-faction registered body: block-or-pass per the filter bit. */
	Friendly = 2,

	/** A hostile registered body: one unified hit submission per passage. */
	Hostile = 3,

	/**
	 * An unregistered body (walls/environment) or an unresolvable hit:
	 * block-or-pass per the filter bit, never a submission (fail-closed).
	 */
	Unregistered = 4
};

/**
 * The shared hit-handling context for one bound pellet (a value snapshot
 * carrying the injected seams; the policy never reads catalogs or source
 * JSON - the attack profile arrives already resolved).
 */
struct FProjectileHitContext
{
	/** The entity registry (faction classification + provenance lookups). */
	const FCombatEntityRegistry* Registry = nullptr;

	/** The hit ledger the unified submissions dedup through. */
	FHitLedger* Ledger = nullptr;

	/** The Actor->EntityId seam (M5-026's IHitscanTargetIdentity, reused). */
	const IHitscanTargetIdentity* Identity = nullptr;

	/** The per-hit attack profile (resolved by the caller; never re-read). */
	FDamageProfile AttackProfile;

	/**
	 * Extra hostile targets the pellet may pass through after the first hit
	 * (0 = terminate on the first; the M5-004 pierce semantics).
	 */
	int32 PierceCount = 0;

	/** True: a same-faction body blocks and terminates the pellet. */
	bool bFriendliesBlockShot = true;

	/** True: an unregistered body (wall) blocks and terminates the pellet. */
	bool bUnregisteredActorsBlockShot = true;

	/**
	 * The shot's source actor (optional but recommended): the bound body
	 * ignores it while moving so the muzzle start never self-blocks.
	 */
	TWeakObjectPtr<AActor> SourceActor;
};

/**
 * Classifies one sweep-hit actor for the bound projectile (the shared frozen
 * face 029..032 reuse). Order: no actor -> None; the source actor (by weak
 * reference or by resolved id) -> Self; an unresolvable/unknown hit ->
 * Unregistered (fail-closed: classification never invents a target); a
 * same-faction registered body -> Friendly; everything else registered ->
 * Hostile. The attacker faction comes from the registry record of
 * Projectile.Context.SourceEntityId (the 026 same-faction pattern).
 */
EProjectileHitClass ClassifyProjectileHitActor(const AActor* HitActor, const ACombatProjectile& Projectile,
	const FProjectileHitContext& Context);

/**
 * The frozen motion-policy seam (029 parabolic, 030 homing and later policies
 * implement exactly this face; the production wiring registers one policy per
 * projectile - never two advancers on one body).
 */
struct IProjectileMotionPolicy
{
	virtual ~IProjectileMotionPolicy() = default;

	/**
	 * Binds one pellet actor. Validates BEFORE any state is taken: the actor
	 * structure (body + single movement component), a finite non-zero
	 * direction and a complete hit context are required; a refusal leaves the
	 * policy finished and binds nothing. The source actor is added to the
	 * body's move-ignore list so the muzzle start never self-blocks.
	 */
	virtual bool Begin(ACombatProjectile& PelletActor, const FVector& InitialDirection,
		const FProjectileHitContext& HitContext) = 0;

	/**
	 * One logical advance per frame (call exactly once per tick): the body is
	 * transported from its previous to its current position by the UE motion
	 * component's swept move. A zero/non-finite DeltaSeconds (Pause-frozen or
	 * degenerate frame) advances nothing. Pass-through classifications resume
	 * the remaining distance inside the same advance; a damage-causing hit
	 * terminates the advance immediately (one hit result per tick at most).
	 */
	virtual void AdvanceMotion(float DeltaSeconds) = 0;

	/** True when the pellet is done (terminated by a hit, or never begun). */
	virtual bool IsFinished() const = 0;
};

/** Hard bound on swept segments inside one AdvanceMotion (pass-through resume steps). */
constexpr int32 MaxProjectileSegmentsPerAdvance = 8;

/**
 * The reference straight-line policy (M5-004's Straight motion): constant
 * velocity along the bound direction, one swept motion-component move per
 * advance, hits classified through the shared face and hostile hits submitted
 * through the unified entry with the complete five-tuple key.
 */
class FLinearProjectilePolicy final : public IProjectileMotionPolicy
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
	 * One swept segment of at most RemainingCm. Returns false when the pellet
	 * terminated (blocked by a blocking classification or the pierce budget
	 * ran out); otherwise OutTraveledCm carries the consumed distance and the
	 * body sits at the resume position.
	 */
	bool AdvanceOneSegment(ACombatProjectile& Actor, float RemainingCm, float& OutTraveledCm);

	/** Pass-through resume: teleports the body just past the hit actor's bounds. */
	float ResumePastHitActor(ACombatProjectile& Actor, const FHitResult& Hit) const;

	/** Submits the hostile hit through the unified entry (complete key). */
	void SubmitUnifiedHit(ACombatProjectile& Actor, const FHitResult& Hit) const;

	/** The bound pellet (invalid after Begin refusal or actor destruction). */
	TWeakObjectPtr<ACombatProjectile> Projectile;

	/** The flight direction exactly as provided (unit per the 025 contract). */
	FVector Direction = FVector::ZeroVector;

	/** The hit context snapshot taken at Begin. */
	FProjectileHitContext Context;

	/** True when the pellet is done (also the not-begun state). */
	bool bFinished = true;

	/** Hostile passages consumed (the pierce budget accounting). */
	int32 PiercedHits = 0;
};

// Compile-time pins: the hit context stays a seam-value snapshot (raw pointers
// are the injected seams only, never owned actors) and the policy face stays
// virtual so 029..032 can only plug in through it.
static_assert(!std::is_pointer_v<decltype(FProjectileHitContext::SourceActor)>, "FProjectileHitContext::SourceActor must stay a weak reference");
static_assert(std::is_abstract_v<IProjectileMotionPolicy>, "IProjectileMotionPolicy must stay an interface");
