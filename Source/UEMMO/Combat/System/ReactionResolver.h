#pragma once

#include "CoreMinimal.h"

#include "DamageTypes.h"
#include "ReactionTypes.h"

#include <type_traits>

/**
 * M5-014: the hit-reaction resolver (M5 interface contract sections 1 and 5).
 * Turns one hit's attack reaction and target policy plus the target's state
 * facts into the "hit received request" - a pure value snapshot describing
 * everything the hit asks the victim's combat component to do: the state
 * gate, the two independent result faces (damage and control), the stun
 * request with its override semantics, the attack interrupt and the impulse
 * (direction included).
 *
 * Contract of ResolveHitReaction:
 * - Pure and stateless: no World, no Actor, no timer, no mutable state. The
 *   resolver never creates an Actor or Component and never owns a second
 *   HitStun/Knockdown timing or state - the CombatComponent stays the only
 *   action-state and timing source and decides whether and how to execute
 *   the request.
 * - The state facts (dead, in landing recovery) are INPUTS the caller reads
 *   from the one state owner. Death has the highest priority: a dead target
 *   refuses the whole hit. A target inside its landing recovery (Knockdown
 *   or Recovering) is get-up protected and refuses the whole hit.
 * - The two result faces are independent and never merged into one bool: a
 *   control-immune target (super armor) takes its full damage with every
 *   control kind refused (bDamageApplies true, bControlAccepted false), and
 *   a damage-immune target takes exactly zero damage with its control
 *   standing (bDamageApplies false, bControlAccepted true). The actual
 *   damage NUMBER and the actual application stay with the M5-011 resolver
 *   and the M5-012 unified entry; this request carries the policy decision.
 * - The stun request carries the max-override semantics: the new stun is
 *   max(remaining, requested), never additive (the M1-020 rule, pinned here
 *   as the request's explicit override mode). The victim's component
 *   executes it on its injected input clock.
 * - The interrupt request is set exactly when the hit would reach the
 *   victim's NotifyHitReceived bridge (damage applied or accepted control):
 *   an accepted hit interrupts the running attack; a refused hit interrupts
 *   nothing.
 * - The impulse request is a world velocity: X is the knockback mirrored by
 *   the attacker facing (+1/-1 mirrors the X axis only), Y is always 0 (the
 *   depth axis is never locked - the execution keeps the target's current
 *   horizontal velocity, the M1-022 bXYOverride=false rule) and Z is the
 *   admitted launch. The float-cycle launch scaling is read from the target
 *   policy (MaxLaunchesPerAirCycle / LaunchZScales); with the legacy default
 *   policy it reproduces the M1-025 constants exactly (1.0 then 0.7, the
 *   third launch of a cycle refused). Only the attacks the caller marks with
 *   FReactionHitContext::bPerCycleLaunchScaling run the scaling (M5-014: the
 *   launcher; aerial_01's compensation stays under the separate M1-024
 *   one-per-cycle gate).
 * - The control kinds are read through the M5-011 ResolveDamage gate summary
 *   (the same computation the unified entry applies), so the request and the
 *   application can never disagree. A knockdown rides an accepted launch
 *   exactly when the knockdown gate allows it (the M5-012 rule).
 * - An illegal damage context (non-finite, negative, non-positive base
 *   damage) is not a state refusal: the request carries no faces and no
 *   magnitudes and the unified entry owns that refusal with its explicit
 *   reason.
 *
 * The default inputs reproduce the M5-013 adaptation path byte for byte:
 * MakeLegacyNormalTargetReaction mints the legacy normal target the
 * component used to hardcode, and with the legacy default policy the request
 * carries the legacy stun forwarding (0.22/0.28/1.0/0.18 s), the launcher's
 * 1.0/0.7/refused Z scaling and the facing-mirrored impulse verbatim.
 */

/**
 * Why the resolver refused the whole hit before anything applies. Append
 * only - never renumber, so logs and tests keep their meaning. 0 always
 * means "not refused by the state gate".
 */
enum class EReactionRefuseReason : uint8
{
	/** The hit was not refused at the state gate. */
	None = 0,

	/** The target is dead: death has the highest priority, nothing applies. */
	TargetDead = 1,

	/** The target is inside its landing recovery (get-up protection): nothing applies. */
	GetUpProtection = 2
};

/**
 * How the requested stun combines with a stun the victim may already be in.
 * Append only - never renumber. Max is the legacy M1-020 semantics and the
 * only mode M5-014 produces.
 */
enum class EHitStunOverride : uint8
{
	/**
	 * The new stun is max(remaining, requested), never additive: a hit during
	 * a stun refreshes to the longer of the two. The victim's component
	 * executes the rule on its injected input clock.
	 */
	Max = 0
};

/**
 * Everything the resolver reads for one hit: the attack's numeric request,
 * the two policy sides (M5-003) and the target's state facts. The facts are
 * read by the caller from the state owners (the victim's health/combat
 * components) - the resolver itself owns no state and touches no World.
 */
struct FReactionHitContext
{
	/**
	 * The attack's per-hit numeric definition (stun, knockback, launch, hit
	 * stop requests). The launch magnitude inside this profile is the
	 * unscaled definition value; the admitted (policy-scaled) launch is
	 * reported on the request.
	 */
	FDamageProfile Attack;

	/** The attack side (M5-003): how hard the attack pushes its control. */
	FAttackReaction AttackReaction;

	/** The target side (M5-003): gates, immunities, float-cycle launch policy, down-state durations. */
	FTargetReaction TargetPolicy;

	/** State fact: the target is dead (death refuses the whole hit first). */
	bool bTargetDead = false;

	/** State fact: the target is inside its landing recovery (get-up protection). */
	bool bTargetInLandingRecovery = false;

	/** Attacker facing (+1 right / -1 left); mirrors the impulse X only, never the Y depth. */
	int32 Facing = 1;

	/**
	 * Launches already applied to the target in its current float cycle (the
	 * target-side count source). Only read when bPerCycleLaunchScaling is
	 * set; a negative count is defensively read as a fresh cycle.
	 */
	int32 LaunchesUsedThisCycle = 0;

	/**
	 * Caller decision: this attack participates in the target's float-cycle
	 * launch policy (M5-014: the launcher only). Attacks without the flag
	 * keep their definition launch verbatim.
	 */
	bool bPerCycleLaunchScaling = false;

	/** The attacker's attack power (the M5-011 damage context; 0 keeps the legacy numbers). */
	float AttackPower = 0.0f;

	/** The target's defense (the M5-011 damage context; 0 keeps the legacy numbers). */
	float Defense = 0.0f;
};

/**
 * The hit-received request: everything one hit asks the victim's combat
 * component to do. A pointer-free value snapshot; the component decides
 * whether and how to execute it (a refused hit executes nothing, a dead
 * body takes no impulse, the stun runs on the victim's own clock).
 */
struct FHitReactionRequest
{
	// ------------------------------------------------------------------
	// State gate: execute nothing while refused (death > get-up protection).
	// ------------------------------------------------------------------

	/** True when the target's state refuses the whole hit (see RefuseReason). */
	bool bRefuseHit = false;

	/** Why the state gate refused the hit; None while bRefuseHit is false. */
	EReactionRefuseReason RefuseReason = EReactionRefuseReason::None;

	// ------------------------------------------------------------------
	// The two independent result faces (never merged into one bool).
	// ------------------------------------------------------------------

	/**
	 * Damage face: the hit's damage part stands (false for a damage-immune
	 * target or a refused/illegal context). The actual number and the
	 * application stay with the M5-011 resolver and the M5-012 entry.
	 */
	bool bDamageApplies = false;

	/**
	 * Control face: at least one control kind was accepted (false for a
	 * control-immune target - the super armor shape - or a refused context).
	 */
	bool bControlAccepted = false;

	// ------------------------------------------------------------------
	// Control kinds (each gated by the target policy through M5-011).
	// ------------------------------------------------------------------

	/** The hit's stagger request passed the target's stagger gate. */
	bool bStagger = false;

	/** The hit's launch request passed the target's launch gate. */
	bool bLaunch = false;

	/** An accepted launch lands into a knockdown exactly when the knockdown gate allows it. */
	bool bKnockdown = false;

	// ------------------------------------------------------------------
	// Stun request (executed by the victim's component on its own clock).
	// ------------------------------------------------------------------

	/** The override semantics against the victim's remaining stun (M5-014: always Max). */
	EHitStunOverride StunOverride = EHitStunOverride::Max;

	/** The requested stun duration in seconds (0 when no stagger applies). */
	float StunSeconds = 0.0f;

	// ------------------------------------------------------------------
	// Attack interrupt request (the legacy CancelCurrentAttack("HitStun")).
	// ------------------------------------------------------------------

	/** True when the accepted hit interrupts the victim's running attack. */
	bool bInterruptAttack = false;

	// ------------------------------------------------------------------
	// Impulse request (executed by the attacker's component on a survivor).
	// ------------------------------------------------------------------

	/**
	 * World-space velocity the hit requests: X = the knockback mirrored by
	 * the facing (the X axis only), Y = 0 (the depth axis is never locked),
	 * Z = the admitted launch (0 when the launch is refused or absent).
	 */
	FVector ImpulseVelocity = FVector::ZeroVector;

	/**
	 * The admitted launch magnitude in cm/s (the definition launch scaled by
	 * the target policy's float-cycle rule; 0 when refused or absent). The
	 * unified request's Attack.LaunchCmPerSecond carries exactly this value.
	 */
	float LaunchCmPerSecond = 0.0f;

	/**
	 * True when the float-cycle launch policy admitted this launch (the
	 * per-cycle gate opened). The caller's cycle bookkeeping counts only
	 * admitted AND applied launches (an applied launch needs the unified
	 * entry's control fact on top).
	 */
	bool bLaunchAdmitted = false;

	// ------------------------------------------------------------------
	// Down-state duration parameters (the target policy's grant; the landing
	// executor keeps the M1-026 constants until M5-016 parameterizes it).
	// ------------------------------------------------------------------

	/** Knockdown duration the policy grants once the target lands launched. */
	float KnockdownSeconds = 0.0f;

	/** Recovering duration the policy grants after the knockdown ends. */
	float RecoveringSeconds = 0.0f;
};

/**
 * M5-014: mints the legacy normal target policy - the exact value the M5-013
 * adaptation hardcoded (M5_013_MakeLegacyTargetReaction): every gate open,
 * no poise, no immunity, 2 launches per cycle at 1.0/0.7, 0.45 s knockdown
 * plus 0.25 s recovering. The adaptation path now lives with the resolver.
 */
inline FTargetReaction MakeLegacyNormalTargetReaction()
{
	FTargetReaction Policy;
	Policy.PolicyId = FName(TEXT("normal"));
	return Policy;
}

/**
 * The float-cycle launch scale the target policy grants launch number
 * LaunchIndex (0-based) of the current cycle: index i reads LaunchZScales[i]
 * clamped to the last entry, an index at or after MaxLaunchesPerAirCycle is
 * refused (0.0). A negative index is defensively read as a fresh cycle
 * (index 0; no caller produces one). A selected entry that is non-finite or
 * not greater than 0 refuses the launch instead of poisoning the impulse.
 * Pure function, no state; with the legacy default policy it reproduces the
 * M1-025 EvaluateAirCombo constants exactly (1.0 / 0.7 / refused).
 */
float ResolveLaunchZScale(const FTargetReaction& TargetPolicy, int32 LaunchIndex);

/**
 * Resolves the hit-received request for one hit (see the header contract
 * above). Pure, stateless, World-free; game thread not required.
 */
FHitReactionRequest ResolveHitReaction(const FReactionHitContext& Context);

// Compile-time pins: the context and the request are value snapshots, never
// World/Actor pointer holders (interface contract section 0.5).
static_assert(!std::is_pointer_v<decltype(FReactionHitContext::Attack)>, "FReactionHitContext::Attack must stay a value type");
static_assert(!std::is_pointer_v<decltype(FReactionHitContext::TargetPolicy)>, "FReactionHitContext::TargetPolicy must stay a value container");
static_assert(!std::is_pointer_v<decltype(FReactionHitContext::Facing)>, "FReactionHitContext::Facing must stay a value type");
static_assert(std::is_default_constructible_v<FReactionHitContext>, "FReactionHitContext must stay default constructible");
static_assert(std::is_trivially_copyable_v<FHitReactionRequest>, "FHitReactionRequest must stay a trivially copyable value snapshot");
static_assert(std::is_default_constructible_v<FHitReactionRequest>, "FHitReactionRequest must stay default constructible");
static_assert(!std::is_pointer_v<decltype(FHitReactionRequest::ImpulseVelocity)>, "FHitReactionRequest::ImpulseVelocity must stay a value type");
static_assert(static_cast<uint8>(EReactionRefuseReason::None) == 0, "EReactionRefuseReason::None must stay 0");
static_assert(static_cast<uint8>(EHitStunOverride::Max) == 0, "EHitStunOverride::Max must stay 0");
